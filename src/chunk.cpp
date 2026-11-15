#include "stream.hpp"

#include "device.hpp"
#include "error.hpp"
#include "feature.hpp"
#include "node_map.hpp"
#include "producer.hpp"

#include <GenApi/ChunkAdapterGeneric.h>

#include <algorithm>
#include <cstring>
#include <set>

using namespace GenTL;
using namespace iris;

namespace {

using Chunks = std::vector<GenApi::SingleChunkData_t>;

void append_chunk(Chunks &chunks, uint64_t id, ptrdiff_t offset, size_t length)
{
    const auto start = static_cast<size_t>(offset);
    const auto end = start + length;
    for (const auto &chunk : chunks) {
        require(chunk.ChunkID != id, IRIS_PRODUCER_ERROR, "chunk decode", "duplicate chunk ID");
        const auto other = static_cast<size_t>(chunk.ChunkOffset);
        require(end <= other || other + chunk.ChunkLength <= start,
                IRIS_PRODUCER_ERROR, "chunk decode", "overlapping chunk ranges");
    }
    chunks.push_back({id, offset, length});
}

Chunks transport_chunks(iris_stream &s, const Buffer &buffer)
{
    auto &p = *s.device->producer;
    const bool multipart = buffer.payload_type == IRIS_PAYLOAD_MULTI_PART;
    require(!multipart || buffer.valid_parts, IRIS_PRODUCER_ERROR, "chunk decode", "invalid multipart descriptors");
    require(p.DSGetBufferChunkData, IRIS_NOT_SUPPORTED, "chunk decode", "producer has no chunk descriptor API");
    size_t count = 0;
    p.check(p.DSGetBufferChunkData(s.handle, buffer.handle, nullptr, &count), "DSGetBufferChunkData");
    require(count && count <= 1024, IRIS_OUT_OF_RANGE, "chunk decode", "invalid chunk count");
    std::vector<SINGLE_CHUNK_DATA> descriptors(count);
    p.check(p.DSGetBufferChunkData(s.handle, buffer.handle, descriptors.data(), &count), "DSGetBufferChunkData");
    require(count && count <= descriptors.size(), IRIS_PRODUCER_ERROR, "chunk decode", "invalid chunk descriptor count");
    Chunks chunks;
    chunks.reserve(count);
    size_t extent = buffer.payload_bound;
    if (!multipart) {
        if (buffer.has_size_filled) extent = buffer.size_filled;
        else if (buffer.has_data_size && (buffer.payload_type == IRIS_PAYLOAD_IMAGE ||
                 buffer.payload_type == IRIS_PAYLOAD_CHUNK_DATA || buffer.payload_type == IRIS_PAYLOAD_CHUNK_ONLY))
            extent = buffer.data_size;
        require(extent <= buffer.payload_bound, IRIS_PRODUCER_ERROR, "chunk decode", "payload exceeds buffer extent");
    }
    size_t bytes_left = multipart && buffer.has_size_filled ? buffer.size_filled : extent;
    for (size_t i = 0; i < buffer.part_count; ++i) bytes_left -= buffer.parts[i].size;
    for (size_t i = 0; i < count; ++i) {
        const auto &chunk = descriptors[i];
        const auto offset = chunk.ChunkOffset;
        require(offset >= 0 && static_cast<size_t>(offset) <= extent &&
                chunk.ChunkLength && chunk.ChunkLength <= extent - offset && chunk.ChunkLength <= bytes_left &&
                static_cast<size_t>(offset) + chunk.ChunkLength <= PTRDIFF_MAX,
                IRIS_PRODUCER_ERROR, "chunk decode", "chunk lies outside filled buffer");
        const auto end = static_cast<size_t>(offset) + chunk.ChunkLength;
        for (size_t j = 0; j < buffer.part_count; ++j) {
            const auto &part = buffer.parts[j];
            const auto start = reinterpret_cast<uintptr_t>(part.data) -
                               reinterpret_cast<uintptr_t>(buffer.payload_base);
            require(!part.size || end <= start || start + part.size <= static_cast<size_t>(offset),
                    IRIS_PRODUCER_ERROR, "chunk decode", "chunk overlaps a data part");
        }
        bytes_left -= chunk.ChunkLength;
        append_chunk(chunks, chunk.ChunkID, offset, chunk.ChunkLength);
    }
    return chunks;
}

Chunks gendc_chunks(const ChunkPart &part)
{
    const auto *data = part.data;
    Chunks chunks;
    size_t end = part.size;
    while (end) {
        require(chunks.size() < 1024, IRIS_OUT_OF_RANGE, "chunk decode", "too many GenDC chunks");
        require(end >= 8, IRIS_PRODUCER_ERROR, "chunk decode", "truncated GenDC chunk tag");
        uint32_t id, length;
        std::memcpy(&id, data + end - 8, 4);
        std::memcpy(&length, data + end - 4, 4);
        require(length && length % 4 == 0 && length <= end - 8, IRIS_PRODUCER_ERROR,
                "chunk decode", "invalid GenDC chunk length");
        end -= length + size_t{8};
        append_chunk(chunks, id, static_cast<ptrdiff_t>(end), length);
    }
    return chunks;
}

enum class ChunkAccess { unrelated, present, missing };

bool is_value_feature(GenApi::INode &node)
{
    const auto type = node.GetPrincipalInterfaceType();
    return type != GenApi::intfICategory && type != GenApi::intfICommand &&
        dynamic_cast<GenApi::IValue *>(&node);
}

ChunkAccess chunk_access(GenApi::INode &node)
{
    GenApi::NodeList_t pending;
    pending.push_back(&node);
    std::set<GenApi::INode *> seen;
    auto access = ChunkAccess::unrelated;
    while (!pending.empty()) {
        auto *current = pending.back();
        pending.pop_back();
        if (!seen.insert(current).second) continue;
        if (auto *port = dynamic_cast<GenApi::IChunkPort *>(current);
            port && !port->GetChunkID().empty()) {
            if (!GenApi::IsReadable(port)) return ChunkAccess::missing;
            access = ChunkAccess::present;
            continue;
        }
        GenApi::NodeList_t children;
        current->GetChildren(children);
        for (auto *child : children) pending.push_back(child);
    }
    return access;
}

template<class F>
iris_status_t with_chunks(iris_stream_t *s, const iris_frame_t *frame, F visit)
{
    if (!s || !frame || !frame->token) return invalid("chunk decode");
    return api("chunk decode", [&] {
        std::lock_guard stream_lock(s->mutex);
        const auto buffer = std::find_if(s->buffers.begin(), s->buffers.end(), [&](const Buffer &entry) {
            return entry.token == frame->token;
        });
        require(buffer != s->buffers.end(), IRIS_INVALID_ARGUMENT, "chunk decode", "frame is not borrowed from this stream");
        require(s->remote_map && buffer->complete,
                IRIS_NOT_SUPPORTED, "chunk decode", "no usable payload or remote node map");
        const auto *data = buffer->payload_base;
        Chunks chunks;
        if (buffer->payload_type == IRIS_PAYLOAD_GENDC) {
            require(buffer->chunk_part.data, IRIS_NOT_SUPPORTED, "chunk decode",
                    "expected one GenDC chunk part with remote XML");
            data = buffer->chunk_part.data;
            chunks = gendc_chunks(buffer->chunk_part);
        } else {
            require(data && (buffer->contains_chunks ||
                    buffer->payload_type == IRIS_PAYLOAD_CHUNK_DATA ||
                    buffer->payload_type == IRIS_PAYLOAD_CHUNK_ONLY),
                    IRIS_NOT_SUPPORTED, "chunk decode", "frame has no chunk data");
            chunks = transport_chunks(*s, *buffer);
        }
        require(!chunks.empty(), IRIS_NOT_FOUND, "chunk decode", "no chunks in payload");

        auto &map = *s->remote_map;
        std::lock_guard feature_lock(*map.mutex);
        GenApi::CChunkAdapterGeneric adapter(map.nodes._Ptr, 0);
        struct Detach {
            GenApi::CChunkAdapterGeneric &adapter;
            ~Detach() { try { adapter.DetachBuffer(); } catch (...) {} }
        } detach{adapter};
        adapter.AttachBuffer(const_cast<uint8_t *>(data), chunks.data(), chunks.size());
        return visit(map.nodes);
    });
}

template<class T, class F>
iris_status_t chunk_feature(iris_stream_t *s, const iris_frame_t *frame, const char *name, F read)
{
    if (!name || !*name) return invalid("chunk decode");
    return with_chunks(s, frame, [&](auto &nodes) {
        auto *node = nodes._GetNode(name);
        require(node, IRIS_NOT_FOUND, "chunk decode", name);
        auto *value = dynamic_cast<T *>(node);
        require(value && is_value_feature(*node),
                IRIS_WRONG_TYPE, "chunk decode", "incompatible chunk feature type");
        const auto access = chunk_access(*node);
        require(access != ChunkAccess::unrelated, IRIS_WRONG_TYPE, "chunk decode", "expected a chunk feature");
        require(access == ChunkAccess::present && GenApi::IsReadable(node),
                IRIS_NOT_FOUND, "chunk decode", "chunk not present or readable");
        return read(*value);
    });
}

} // namespace

iris_status_t iris_frame_chunk_list(iris_stream_t *s, const iris_frame_t *frame, iris_feature_list_t **out)
{
    if (!out) return invalid("chunk list");
    *out = nullptr;
    return with_chunks(s, frame, [&](auto &nodes) {
        GenApi::NodeList_t entries;
        nodes._GetNodes(entries);
        auto list = std::make_unique<iris_feature_list>();
        for (auto *node : entries) {
            if (!is_value_feature(*node) || chunk_access(*node) != ChunkAccess::present ||
                !GenApi::IsReadable(node))
                continue;
            list->entries.push_back(describe_entry(*node));
        }
        std::sort(list->entries.begin(), list->entries.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
        });
        *out = list.release();
        return IRIS_OK;
    });
}

#define CHUNK_SCALAR(name, iface, type) \
iris_status_t iris_frame_chunk_get_##name(iris_stream_t *s, const iris_frame_t *frame, const char *n, type *out) \
{ \
    if (!out) return invalid("chunk decode"); \
    return chunk_feature<GenApi::iface>(s, frame, n, [&](auto &value) { \
        *out = value.GetValue(true, true); \
        return IRIS_OK; \
    }); \
}
CHUNK_SCALAR(integer, IInteger, int64_t)
CHUNK_SCALAR(float, IFloat, double)
CHUNK_SCALAR(boolean, IBoolean, uint32_t)
#undef CHUNK_SCALAR

#define CHUNK_TEXT(name, iface, expression) \
iris_status_t iris_frame_chunk_##name(iris_stream_t *s, const iris_frame_t *frame, const char *n, char *out, size_t *size) \
{ \
    if (!size || (!out && *size)) return invalid("chunk decode"); \
    return chunk_feature<GenApi::iface>(s, frame, n, [&](auto &value) { \
        return copy_text((expression).c_str(), out, size); \
    }); \
}
CHUNK_TEXT(to_string, IValue, value.ToString(true, true))
CHUNK_TEXT(get_string, IString, value.GetValue(true, true))
#undef CHUNK_TEXT

iris_status_t iris_frame_chunk_get_enumeration(iris_stream_t *s, const iris_frame_t *frame,
                                               const char *name, char *out, size_t *size)
{
    if (!size || (!out && *size)) return invalid("chunk decode");
    return chunk_feature<GenApi::IEnumeration>(s, frame, name, [&](auto &value) {
        auto *entry = value.GetCurrentEntry(true, true);
        require(entry, IRIS_NOT_FOUND, "chunk decode", "chunk enumeration entry not found");
        return copy_text(entry->GetSymbolic().c_str(), out, size);
    });
}

iris_status_t iris_frame_chunk_read_register(iris_stream_t *s, const iris_frame_t *frame,
                                             const char *name, void *out, size_t *size)
{
    if (!size || (!out && *size)) return invalid("chunk decode");
    return chunk_feature<GenApi::IRegister>(s, frame, name, [&](auto &reg) {
        return read_register(reg, out, size, "chunk decode");
    });
}
