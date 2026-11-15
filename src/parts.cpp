#include "stream.hpp"

#include "device.hpp"
#include "gentl_info.hpp"
#include "producer.hpp"

#include <cstring>

using namespace GenTL;
using namespace iris;

namespace {

bool contains(size_t extent, uint64_t offset, uint64_t size)
{
    return offset <= extent && size <= extent - offset;
}

struct Range {
    uint64_t begin, end;
};

template<size_t N>
bool add_range(std::array<Range, N> &ranges, size_t &count, uint64_t offset, uint64_t size)
{
    if (!size) return true;
    if (count == N) return false;
    const auto end = offset + size; // The caller has checked the enclosing extent.
    for (size_t i = 0; i < count; ++i)
        if (offset < ranges[i].end && ranges[i].begin < end) return false;
    ranges[count++] = {offset, end};
    return true;
}

iris_status_t multipart(iris_stream &s, Buffer &buffer, iris_frame_t &frame,
                        const unsigned char *data_base, size_t capacity)
{
    auto &p = *s.device->producer;
    if (!p.DSGetNumBufferParts || !p.DSGetBufferPartInfo) return IRIS_NOT_SUPPORTED;
    uint32_t count = 0;
    const auto result = p.DSGetNumBufferParts(s.handle, buffer.handle, &count);
    if (result != GC_ERR_SUCCESS) return p.error(result, "DSGetNumBufferParts");
    if (count > buffer.parts.size()) return IRIS_OUT_OF_RANGE;

    const auto base = reinterpret_cast<uintptr_t>(data_base);
    size_t bytes_left = (frame.payload.valid & IRIS_PAYLOAD_HAS_SIZE_FILLED)
        ? frame.payload.size_filled : capacity;
    std::array<Range, IRIS_MAX_FRAME_PARTS> ranges;
    size_t range_count = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto &part = buffer.parts[i];
        part = {};
        const auto get = [&](BUFFER_PART_INFO_CMD cmd, auto &out) {
            return get_info(out, [&](auto *type, auto *data, auto *size) {
                return p.DSGetBufferPartInfo(s.handle, buffer.handle, i, cmd, type, data, size);
            });
        };
        void *data = nullptr;
        if (!get(BUFFER_PART_INFO_BASE, data) || !data ||
            !get(BUFFER_PART_INFO_DATA_SIZE, part.size) || !get(BUFFER_PART_INFO_DATA_TYPE, part.type))
            return IRIS_PRODUCER_ERROR;
        const auto address = reinterpret_cast<uintptr_t>(data);
        if (address < base || !contains(capacity, address - base, part.size) ||
            part.size > bytes_left || !add_range(ranges, range_count, address - base, part.size))
            return IRIS_PRODUCER_ERROR;
        bytes_left -= part.size;
        part.data = data;
        if (get(BUFFER_PART_INFO_DATA_FORMAT, part.format) &&
            get(BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE, part.format_namespace))
            part.valid |= IRIS_PART_HAS_FORMAT;
        if (get(BUFFER_PART_INFO_WIDTH, part.width)) part.valid |= IRIS_PART_HAS_WIDTH;
        if (get(BUFFER_PART_INFO_HEIGHT, part.height)) part.valid |= IRIS_PART_HAS_HEIGHT;
        if (get(BUFFER_PART_INFO_SOURCE_ID, part.source_id)) part.valid |= IRIS_PART_HAS_SOURCE_ID;
        if (get(BUFFER_PART_INFO_REGION_ID, part.region_id)) part.valid |= IRIS_PART_HAS_REGION_ID;
        if (get(BUFFER_PART_INFO_XPADDING, part.x_padding)) part.valid |= IRIS_PART_HAS_X_PADDING;
        if (get(BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT, part.delivered_height))
            part.valid |= IRIS_PART_HAS_DELIVERED_HEIGHT;
    }
    frame.part_count = count;
    return IRIS_OK;
}

// GenDC 1.x descriptor tables are little-endian. Read unaligned wire fields
// with memcpy on the Linux x86-64 target, after checking their enclosing header.
template<class T> T read(const unsigned char *header, size_t offset)
{
    T value;
    std::memcpy(&value, header + offset, sizeof(value));
    return value;
}

iris_status_t gendc(Buffer &buffer, iris_frame_t &frame, const unsigned char *base, size_t capacity)
{
    constexpr size_t container_header = 56, component_header = 48, part_header = 40;
    const auto filled = (frame.payload.valid & IRIS_PAYLOAD_HAS_SIZE_FILLED)
        ? frame.payload.size_filled : capacity;
    if (filled < container_header || std::memcmp(base, "GNDC", 4) != 0)
        return IRIS_PRODUCER_ERROR;
    if (base[4] != 1) return IRIS_NOT_SUPPORTED;
    if (read<uint16_t>(base, 24) != 0) return IRIS_NOT_SUPPORTED; // Prefetch or preliminary descriptor.

    const auto header_size = read<uint32_t>(base, 12);
    const auto data_size = read<uint64_t>(base, 32);
    const auto data_offset = read<uint64_t>(base, 40);
    const auto descriptor_size = read<uint32_t>(base, 48);
    const auto components = read<uint32_t>(base, 52);
    if (read<uint16_t>(base, 8) != 0x1000 || !components ||
        !contains(header_size, container_header, uint64_t{components} * 8) ||
        header_size > descriptor_size || descriptor_size > filled || descriptor_size > data_offset ||
        !contains(capacity, data_offset, 0))
        return IRIS_PRODUCER_ERROR;
    if (components > buffer.parts.size()) return IRIS_OUT_OF_RANGE;

    size_t count = 0;
    size_t visited = 0;
    size_t chunk_parts = 0;
    bool embedded_chunk_xml = false;
    size_t bytes_left = filled - descriptor_size;
    std::array<Range, 2 * IRIS_MAX_FRAME_PARTS> headers;
    std::array<Range, IRIS_MAX_FRAME_PARTS> data_ranges;
    size_t header_count = 0, range_count = 0;
    for (uint32_t c = 0; c < components; ++c) {
        const auto offset = read<uint64_t>(base, container_header + c * 8);
        if (offset < header_size || !contains(descriptor_size, offset, component_header))
            return IRIS_PRODUCER_ERROR;
        const auto *component = base + offset;
        if (read<uint16_t>(component, 0) != 0x2000) return IRIS_PRODUCER_ERROR;
        const auto size = read<uint32_t>(component, 4);
        const auto parts = read<uint16_t>(component, 46);
        if (size < component_header || !contains(descriptor_size, offset, size) ||
            !add_range(headers, header_count, offset, size))
            return IRIS_PRODUCER_ERROR;
        if (read<uint16_t>(component, 2) & 1) continue; // Invalid component.
        if (!parts || !contains(size, component_header, uint64_t{parts} * 8))
            return IRIS_PRODUCER_ERROR;
        visited += parts;
        if (visited > buffer.parts.size()) return IRIS_OUT_OF_RANGE;

        for (uint16_t i = 0; i < parts; ++i) {
            const auto part_offset = read<uint64_t>(component, component_header + i * 8);
            if (part_offset < header_size || !contains(descriptor_size, part_offset, part_header))
                return IRIS_PRODUCER_ERROR;
            const auto *header = base + part_offset;
            const auto type = read<uint16_t>(header, 0);
            const auto bytes = read<uint32_t>(header, 4);
            const auto length = read<uint64_t>(header, 24);
            const auto data = read<uint64_t>(header, 32);
            if ((type & 0xf000) != 0x4000 || bytes < part_header ||
                !contains(descriptor_size, part_offset, bytes) || data < data_offset ||
                !contains(data_size, data - data_offset, length) ||
                !contains(capacity, data, length) || length > bytes_left ||
                !add_range(headers, header_count, part_offset, bytes) ||
                !add_range(data_ranges, range_count, data, length))
                return IRIS_PRODUCER_ERROR;
            bytes_left -= length;
            auto &part = buffer.parts[count++];
            part = {};
            part.data = base + data;
            part.size = length;
            part.type = type;
            part.format = read<uint32_t>(header, 8);
            part.format_namespace = PIXELFORMAT_NAMESPACE_PFNC_32BIT;
            part.source_id = read<uint16_t>(component, 12);
            part.region_id = read<uint16_t>(component, 14);
            part.component_type = read<uint64_t>(component, 32);
            part.valid = IRIS_PART_HAS_FORMAT | IRIS_PART_HAS_SOURCE_ID |
                         IRIS_PART_HAS_REGION_ID | IRIS_PART_HAS_COMPONENT_TYPE;
            if (type == 0x4000) { // GDC_METADATA_GENICAM_CHUNK
                if (bytes < 64 || part.format != 0x01080116) return IRIS_PRODUCER_ERROR; // PFNC Data8
                const auto chunk_size = read<uint64_t>(header, 40);
                const auto padding = read<uint16_t>(header, 48);
                if (chunk_size > length || padding != length - chunk_size)
                    return IRIS_PRODUCER_ERROR;
                ++chunk_parts;
                if (chunk_parts == 1) {
                    buffer.chunk_part = {base + data, chunk_size};
                } else {
                    buffer.chunk_part = {};
                }
            }
            if (type == 0x4001 && (read<uint16_t>(header, 2) & (1u << 9)))
                embedded_chunk_xml = true;
            if (type >= 0x4200 && type <= 0x4204) {
                if (bytes < (type == 0x4203 ? 72u : 56u)) return IRIS_PRODUCER_ERROR;
                part.width = read<uint32_t>(header, 40);
                part.height = read<uint32_t>(header, 44);
                part.x_padding = read<uint16_t>(header, 48);
                part.valid |= IRIS_PART_HAS_WIDTH | IRIS_PART_HAS_HEIGHT | IRIS_PART_HAS_X_PADDING;
            }
        }
    }
    if (embedded_chunk_xml) buffer.chunk_part = {};
    frame.part_count = count;
    return IRIS_OK;
}

} // namespace

void iris::inspect_parts(iris_stream &s, Buffer &buffer, iris_frame_t &frame,
                         const unsigned char *base, size_t capacity)
{
    buffer.chunk_part = {};
    if (frame.payload.valid & IRIS_PAYLOAD_IS_COMPOSITE) {
        frame.parts_status = IRIS_NOT_SUPPORTED;
        return;
    }
    if (frame.status == IRIS_FRAME_INCOMPLETE ||
        !(frame.payload.valid & IRIS_PAYLOAD_HAS_INCOMPLETE) || !base) {
        frame.parts_status = IRIS_INVALID_STATE;
        return;
    }
    frame.parts_status = frame.payload.type == IRIS_PAYLOAD_MULTI_PART
        ? multipart(s, buffer, frame, base, capacity) : gendc(buffer, frame, base, capacity);
    if (frame.parts_status == IRIS_OK) {
        if (frame.part_count) frame.parts = buffer.parts.data();
    } else {
        buffer.chunk_part = {};
        frame.part_count = 0;
    }
}
