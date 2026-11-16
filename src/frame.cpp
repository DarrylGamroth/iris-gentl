#include "stream.hpp"

#include "device.hpp"
#include "error.hpp"
#include "gentl_info.hpp"
#include "producer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#ifdef IRIS_TRACE_INVALID_IMAGE
#include <cstdio>
#endif

#include <time.h>

using namespace GenTL;
using namespace iris;

namespace {

alignas(cache_line_size) std::atomic<uint64_t> next_token{1};

template<class T>
bool buffer_info(iris_stream &s, BUFFER_HANDLE buffer, BUFFER_INFO_CMD cmd, T &out)
{
    return get_info(out, [&](auto *type, auto *data, auto *size) {
        return s.device->producer->DSGetBufferInfo(s.handle, buffer, cmd, type, data, size);
    });
}

struct ScalarBufferInfo {
    iris_stream &stream;
    BUFFER_HANDLE handle;

    template<BUFFER_INFO_CMD command, class T> bool get(T &out) const
    {
        return buffer_info(stream, handle, command, out);
    }
};

struct BufferInfoField {
    BUFFER_INFO_CMD command;
    size_t size;
};

constexpr BufferInfoField frame_info_fields[] = {
    {BUFFER_INFO_FRAMEID, sizeof(uint64_t)},
    {BUFFER_INFO_TIMESTAMP_NS, sizeof(uint64_t)},
    {BUFFER_INFO_PIXEL_ENDIANNESS, sizeof(int32_t)},
    {BUFFER_INFO_IS_INCOMPLETE, sizeof(bool8_t)},
    {BUFFER_INFO_PAYLOADTYPE, sizeof(size_t)},
    {BUFFER_INFO_SIZE_FILLED, sizeof(size_t)},
    {BUFFER_INFO_DATA_SIZE, sizeof(size_t)},
    {BUFFER_INFO_IMAGEOFFSET, sizeof(size_t)},
    {BUFFER_INFO_DELIVERED_IMAGEHEIGHT, sizeof(size_t)},
    {BUFFER_INFO_CONTAINS_CHUNKDATA, sizeof(bool8_t)},
    {BUFFER_INFO_IS_COMPOSITE, sizeof(bool8_t)},
    {BUFFER_INFO_BASE, sizeof(void *)},
    {BUFFER_INFO_SIZE, sizeof(size_t)},
    {BUFFER_INFO_DATA_LARGER_THAN_BUFFER, sizeof(bool8_t)},
    {BUFFER_INFO_PIXELFORMAT, sizeof(uint64_t)},
    {BUFFER_INFO_PIXELFORMAT_NAMESPACE, sizeof(uint64_t)},
    {BUFFER_INFO_WIDTH, sizeof(size_t)},
    {BUFFER_INFO_HEIGHT, sizeof(size_t)},
    {BUFFER_INFO_IMAGEPRESENT, sizeof(bool8_t)},
    {BUFFER_INFO_XPADDING, sizeof(size_t)},
};

constexpr size_t frame_info_count = std::size(frame_info_fields);

constexpr size_t frame_info_index(BUFFER_INFO_CMD command)
{
    for (size_t i = 0; i < frame_info_count; ++i)
        if (frame_info_fields[i].command == command) return i;
    return frame_info_count;
}

struct StackedBufferInfo {
    ScalarBufferInfo scalar;
    alignas(uint64_t) unsigned char values[frame_info_count][sizeof(uint64_t)]{};
    DS_BUFFER_INFO_STACKED entries[frame_info_count];

    GC_ERROR query(PDSGetBufferInfoStacked call)
    {
        for (size_t i = 0; i < frame_info_count; ++i) {
            const auto field = frame_info_fields[i];
            entries[i] = {field.command, INFO_DATATYPE_UNKNOWN, values[i], field.size, GC_ERR_NOT_AVAILABLE};
        }
        return call(scalar.stream.handle, scalar.handle, entries, frame_info_count);
    }

    template<BUFFER_INFO_CMD command, class T> bool get(T &out) const
    {
        constexpr auto index = frame_info_index(command);
        if constexpr (index == frame_info_count) {
            static_assert(command == BUFFER_INFO_CHUNKLAYOUTID);
            return scalar.get<command>(out);
        } else {
            static_assert(frame_info_fields[index].size == sizeof(T));
            const auto &entry = entries[index];
            if (entry.iResult != GC_ERR_SUCCESS || !valid_info<T>(entry.iType, entry.iSize)) return false;
            std::memcpy(&out, values[index], sizeof(T));
            return true;
        }
    }
};

// PFNC single-plane formats with a known row layout. Packed Mono formats
// require an integral number of groups per row, so image and line packing
// produce the same row boundaries.
size_t row_bytes(uint64_t format, size_t width)
{
    size_t pixels = 1, bytes = 0;
    switch (format) {
    case 0x01080001: // Mono8
    case 0x01080002: // Mono8s
    case 0x01080008: // BayerGR8
    case 0x01080009: // BayerRG8
    case 0x0108000A: // BayerGB8
    case 0x0108000B: // BayerBG8
        bytes = 1; break;
    case 0x01100003: // Mono10
    case 0x01100005: // Mono12
    case 0x01100007: // Mono16
    case 0x01100025: // Mono14
        bytes = 2; break;
    case 0x02180014: // RGB8
    case 0x02180015: // BGR8
        bytes = 3; break;
    case 0x02200016: // RGBa8
    case 0x02200017: // BGRa8
        bytes = 4; break;
    case 0x010A0046: pixels = 4; bytes = 5; break; // Mono10p
    case 0x010C0047: pixels = 2; bytes = 3; break; // Mono12p
    case 0x010E0104: pixels = 4; bytes = 7; break; // Mono14p
    default: return 0;
    }
    if (width % pixels || width / pixels > SIZE_MAX / bytes) return 0;
    return width / pixels * bytes;
}

template<class Info> void image(iris_stream &s, Buffer &buffer, iris_frame_t &f, const Info &info)
{
    if (info.template get<BUFFER_INFO_FRAMEID>(f.frame_id)) f.valid |= IRIS_FRAME_HAS_ID;
    if (info.template get<BUFFER_INFO_TIMESTAMP_NS>(f.timestamp_ns)) f.valid |= IRIS_FRAME_HAS_TIMESTAMP_NS;
    if (info.template get<BUFFER_INFO_PIXEL_ENDIANNESS>(f.pixel_endianness)) f.valid |= IRIS_FRAME_HAS_PIXEL_ENDIANNESS;
    auto &payload = f.payload;
    f.user_data = buffer.user_data;
    payload.capacity = buffer.capacity;
    bool8_t incomplete = 0, larger = 0, chunks = 0;
    if (info.template get<BUFFER_INFO_IS_INCOMPLETE>(incomplete))
        payload.valid |= IRIS_PAYLOAD_HAS_INCOMPLETE;
    if (info.template get<BUFFER_INFO_PAYLOADTYPE>(payload.type))
        payload.valid |= IRIS_PAYLOAD_HAS_TYPE;
    if (info.template get<BUFFER_INFO_SIZE_FILLED>(payload.size_filled))
        payload.valid |= IRIS_PAYLOAD_HAS_SIZE_FILLED;
    if (info.template get<BUFFER_INFO_DATA_SIZE>(payload.data_size))
        payload.valid |= IRIS_PAYLOAD_HAS_DATA_SIZE;
    if (info.template get<BUFFER_INFO_IMAGEOFFSET>(payload.image_offset))
        payload.valid |= IRIS_PAYLOAD_HAS_IMAGE_OFFSET;
    if (info.template get<BUFFER_INFO_DELIVERED_IMAGEHEIGHT>(payload.delivered_image_height))
        payload.valid |= IRIS_PAYLOAD_HAS_DELIVERED_IMAGE_HEIGHT;
    if (info.template get<BUFFER_INFO_CONTAINS_CHUNKDATA>(chunks))
        payload.valid |= IRIS_PAYLOAD_HAS_CHUNK_DATA;
    if ((chunks || payload.type == IRIS_PAYLOAD_CHUNK_DATA || payload.type == IRIS_PAYLOAD_CHUNK_ONLY) &&
        info.template get<BUFFER_INFO_CHUNKLAYOUTID>(payload.chunk_layout_id))
        payload.valid |= IRIS_PAYLOAD_HAS_CHUNK_LAYOUT_ID;
    payload.incomplete = incomplete != 0;
    payload.contains_chunk_data = chunks != 0;
    // GenTL composite offsets can refer to discontiguous memory.
    bool8_t composite = 0;
    info.template get<BUFFER_INFO_IS_COMPOSITE>(composite);
    if (composite) payload.valid |= IRIS_PAYLOAD_IS_COMPOSITE;

    void *base = nullptr;
    size_t reported_capacity = buffer.capacity;
    const auto filled = payload.size_filled;
    const auto *announced = buffer.caller_data ? buffer.caller_data : buffer.storage;
    const bool has_base = !composite && filled <= buffer.capacity &&
        info.template get<BUFFER_INFO_BASE>(base) && base &&
        (!announced || base == announced) &&
        (!info.template get<BUFFER_INFO_SIZE>(reported_capacity) || filled <= reported_capacity);
    const size_t capacity = std::min(buffer.capacity, reported_capacity);
    const bool structured = payload.type == IRIS_PAYLOAD_MULTI_PART || payload.type == IRIS_PAYLOAD_GENDC;
    const bool truncated = incomplete || (info.template get<BUFFER_INFO_DATA_LARGER_THAN_BUFFER>(larger) && larger);
    const bool bounded = has_base && capacity && capacity <= PTRDIFF_MAX &&
        capacity <= UINTPTR_MAX - reinterpret_cast<uintptr_t>(base);
    buffer.payload_base = bounded ? static_cast<const unsigned char *>(base) : nullptr;
    buffer.payload_bound = bounded ? capacity : 0;
    if (structured) {
        f.status = truncated ? IRIS_FRAME_INCOMPLETE : IRIS_FRAME_UNSUPPORTED_PAYLOAD;
        inspect_parts(s, buffer, f, bounded ? static_cast<const unsigned char *>(base) : nullptr, capacity);
        return;
    }
    if (bounded && filled) payload.data = base;
    if (truncated) {
        f.status = IRIS_FRAME_INCOMPLETE;
        return;
    }
    if (!(payload.valid & IRIS_PAYLOAD_HAS_TYPE) || payload.type != IRIS_PAYLOAD_IMAGE) {
        f.status = IRIS_FRAME_UNSUPPORTED_PAYLOAD;
        return;
    }
    f.status = IRIS_FRAME_INVALID_IMAGE;
    uint64_t name_space = 0;
    if (info.template get<BUFFER_INFO_PIXELFORMAT>(f.pixel_format) &&
        info.template get<BUFFER_INFO_PIXELFORMAT_NAMESPACE>(name_space) && name_space <= UINT32_MAX) {
        f.valid |= IRIS_FRAME_HAS_PIXEL_FORMAT;
        f.pixel_format_namespace = static_cast<uint32_t>(name_space);
    }
    const auto offset = payload.image_offset;
    bool8_t present = 0;
    if (!(payload.valid & IRIS_PAYLOAD_HAS_INCOMPLETE) || !payload.data ||
        !(payload.valid & IRIS_PAYLOAD_HAS_IMAGE_OFFSET) || offset >= filled ||
        !info.template get<BUFFER_INFO_WIDTH>(f.width) ||
        !info.template get<BUFFER_INFO_HEIGHT>(f.height) ||
        (info.template get<BUFFER_INFO_IMAGEPRESENT>(present) && !present) ||
        !f.width || !f.height)
        return;
    if (!(f.valid & IRIS_FRAME_HAS_PIXEL_FORMAT) ||
        (name_space != PIXELFORMAT_NAMESPACE_PFNC_16BIT &&
         name_space != PIXELFORMAT_NAMESPACE_PFNC_32BIT &&
         name_space != PIXELFORMAT_NAMESPACE_GEV))
        return;
    const size_t row = row_bytes(f.pixel_format, f.width);
    if (!row) {
        f.status = IRIS_FRAME_UNSUPPORTED_PAYLOAD;
        return;
    }
    if (payload.valid & IRIS_PAYLOAD_HAS_DELIVERED_IMAGE_HEIGHT) {
        const auto delivered_height = payload.delivered_image_height;
        if (delivered_height > f.height) return;
        if (delivered_height < f.height) {
            f.status = IRIS_FRAME_INCOMPLETE;
            return;
        }
    }
    size_t xpad = 0;
    if (!info.template get<BUFFER_INFO_XPADDING>(xpad) || xpad > SIZE_MAX - row)
        return;
    const size_t stride = row + xpad;
    if (f.height > (filled - offset) / stride) {
        // Some producers report a short image with IS_INCOMPLETE clear.
        f.status = IRIS_FRAME_INCOMPLETE;
        return;
    }
    f.stride_bytes = stride;
    f.image_size = stride * f.height;
    f.data = static_cast<const unsigned char *>(base) + offset;
    f.status = IRIS_FRAME_COMPLETE;
    f.valid |= IRIS_FRAME_HAS_STRIDE;
}

// Keep the batch workspace off the scalar polling path.
[[gnu::noinline]] bool stacked_image(iris_stream &s, Buffer &buffer, iris_frame_t &f,
                                    PDSGetBufferInfoStacked call)
{
    StackedBufferInfo info{{s, buffer.handle}};
    const auto status = info.query(call);
    if (status == GC_ERR_SUCCESS) {
        image(s, buffer, f, info);
        return true;
    }
    if (status == GC_ERR_NOT_IMPLEMENTED) s.DSGetBufferInfoStacked = nullptr;
    return false;
}

void image(iris_stream &s, Buffer &buffer, iris_frame_t &f)
{
    if (s.DSGetBufferInfoStacked && stacked_image(s, buffer, f, s.DSGetBufferInfoStacked)) return;
    image(s, buffer, f, ScalarBufferInfo{s, buffer.handle});
}

class Reader {
    iris_stream &stream;
public:
    explicit Reader(iris_stream &s) : stream(s) {}
    ~Reader()
    {
        std::lock_guard lock(stream.mutex);
        stream.reader = false;
        stream.idle.notify_all();
    }
};

iris_status_t begin_read(iris_stream &s)
{
    std::lock_guard lock(s.mutex);
    if (s.reader || s.partial.buffer) return fail(IRIS_BUSY, "receive", "reader already active");
    if (!s.running) return IRIS_STOPPED;
    s.reader = true;
    return IRIS_OK;
}

iris_status_t receive(iris_stream &s, uint64_t timeout, iris_frame_t &f,
                      std::unique_lock<std::mutex> &lock, Buffer **source = nullptr)
{
    auto &p = *s.device->producer;
    EVENT_NEW_BUFFER_DATA event{};
    size_t size = sizeof(event);
    GC_ERROR rc;
    const auto started = timeout ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    auto remaining = timeout;
    do {
        const auto slice = std::min<uint64_t>(remaining, 20);
        size = sizeof(event);
        const auto status = api("EventGetData", [&] {
            rc = p.EventGetData(s.event, &event, &size, slice);
            return IRIS_OK;
        });
        if (status < 0) {
            if (!lock.owns_lock()) lock.lock();
            s.running = false;
            s.stopping = true;
            return status;
        }
        if (rc != GC_ERR_TIMEOUT) break;
        if (!timeout) return IRIS_NO_FRAME;
        {
            if (!lock.owns_lock()) lock.lock();
            if (!s.running) return IRIS_STOPPED;
            lock.unlock();
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        remaining = static_cast<uint64_t>(elapsed) < timeout ? timeout - elapsed : 0;
    } while (remaining);
    if (rc == GC_ERR_TIMEOUT) return IRIS_TIMEOUT;
    if (rc == GC_ERR_ABORT) {
        if (!lock.owns_lock()) lock.lock();
        if (!s.running) return IRIS_STOPPED;
    }
    if (rc != GC_ERR_SUCCESS) return p.error(rc, "EventGetData");
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        f.host_timestamp_ns = uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
        f.valid |= IRIS_FRAME_HAS_HOST_TIMESTAMP_NS;
    }
    if (!lock.owns_lock()) lock.lock();
    auto it = std::find_if(s.buffers.begin(), s.buffers.end(), [&](const Buffer &b) {
        return b.handle == event.BufferHandle;
    });
    if (size != sizeof(event) || !event.BufferHandle || it == s.buffers.end()) {
        s.running = false;
        s.stopping = true;
        return fail(IRIS_PRODUCER_ERROR, "EventGetData", "invalid buffer event");
    }
    if (it->token) {
        s.running = false;
        s.stopping = true;
        return fail(IRIS_PRODUCER_ERROR, "EventGetData", "buffer already borrowed");
    }
    if (!s.running) return IRIS_STOPPED;
    const auto inspected = api("DSGetBufferInfo", [&] {
        image(s, *it, f);
        return IRIS_OK;
    });
    if (inspected < 0) {
        s.running = false;
        s.stopping = true;
        return inspected;
    }
#ifdef IRIS_TRACE_INVALID_IMAGE
    if (f.status == IRIS_FRAME_INVALID_IMAGE || f.status == IRIS_FRAME_INCOMPLETE) {
        constexpr BUFFER_INFO_CMD fields[] = {
            BUFFER_INFO_BASE, BUFFER_INFO_SIZE, BUFFER_INFO_SIZE_FILLED,
            BUFFER_INFO_DATA_SIZE, BUFFER_INFO_IMAGEOFFSET, BUFFER_INFO_WIDTH,
            BUFFER_INFO_HEIGHT, BUFFER_INFO_DELIVERED_IMAGEHEIGHT,
            BUFFER_INFO_IMAGEPRESENT, BUFFER_INFO_XPADDING, BUFFER_INFO_YPADDING,
            BUFFER_INFO_IS_INCOMPLETE, BUFFER_INFO_DATA_LARGER_THAN_BUFFER,
            BUFFER_INFO_PAYLOADTYPE, BUFFER_INFO_PIXELFORMAT,
            BUFFER_INFO_PIXELFORMAT_NAMESPACE, BUFFER_INFO_FRAMEID,
            BUFFER_INFO_TIMESTAMP_NS
        };
        std::fprintf(stderr, "rejected image: status=%d announced=%zu frame=%llu buffer=%p\n",
                     f.status, it->capacity, static_cast<unsigned long long>(f.frame_id), it->handle);
        for (const auto field : fields) {
            uint64_t value = 0;
            size_t bytes = sizeof(value);
            INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
            const auto result = p.DSGetBufferInfo(s.handle, it->handle, field, &type, &value, &bytes);
            std::fprintf(stderr, "  command=%d rc=%d type=%d bytes=%zu value=%llu\n",
                         field, result, type, bytes, static_cast<unsigned long long>(value));
        }
    }
#endif
    it->token = next_token.fetch_add(1, std::memory_order_relaxed);
    f.token = it->token;
    it->size_filled = f.payload.size_filled;
    it->data_size = f.payload.data_size;
    it->payload_type = f.payload.type;
    it->has_size_filled = f.payload.valid & IRIS_PAYLOAD_HAS_SIZE_FILLED;
    it->has_data_size = f.payload.valid & IRIS_PAYLOAD_HAS_DATA_SIZE;
    it->complete = (f.payload.valid & IRIS_PAYLOAD_HAS_INCOMPLETE) && f.status != IRIS_FRAME_INCOMPLETE;
    it->contains_chunks = f.payload.contains_chunk_data;
    it->part_count = f.part_count;
    it->valid_parts = f.parts_status == IRIS_OK;
    if (source) *source = &*it;
    ++s.stats.outstanding_frames;
    ++s.stats.delivered_frames;
    if (f.status == IRIS_FRAME_INCOMPLETE) ++s.stats.incomplete_frames;
    if (f.status == IRIS_FRAME_INVALID_IMAGE) ++s.stats.invalid_images;
    if (f.status == IRIS_FRAME_UNSUPPORTED_PAYLOAD) ++s.stats.unsupported_payloads;
    return IRIS_OK;
}

iris_status_t get_frame(iris_stream_t *s, uint64_t timeout, iris_frame_t *out)
{
    if (!out) return invalid("receive");
    *out = {};
    if (!s) return invalid("receive");
    if (!timeout) {
        std::unique_lock lock(s->mutex, std::try_to_lock);
        if (!lock || s->reader || s->partial.buffer) return fail(IRIS_BUSY, "receive", "stream reader busy");
        if (!s->running) return IRIS_STOPPED;
        const auto status = receive(*s, 0, *out, lock);
        if (status != IRIS_OK) *out = {};
        return status;
    }
    auto status = begin_read(*s);
    if (status != IRIS_OK) return status;
    Reader reader(*s);
    std::unique_lock lock(s->mutex, std::defer_lock);
    status = receive(*s, timeout, *out, lock);
    if (status != IRIS_OK) *out = {};
    return status;
}
}

iris_status_t iris_stream_try_get_frame(iris_stream_t *s, iris_frame_t *out)
{
    return api("try frame", [&]() -> iris_status_t {
        return get_frame(s, 0, out);
    });
}
iris_status_t iris_stream_wait_frame(iris_stream_t *s, uint64_t timeout, iris_frame_t *out)
{
    return api("wait frame", [&]() -> iris_status_t {
        if (!timeout || timeout == GENTL_INFINITE) {
            if (out) *out = {};
            return invalid("wait frame");
        }
        return get_frame(s, timeout, out);
    });
}
namespace {

iris_status_t release_frame(iris_stream &s, iris_frame_t &frame,
                            std::unique_lock<std::mutex> &lock, bool keep_lock)
{
    auto it = std::find_if(s.buffers.begin(), s.buffers.end(), [&](const Buffer &b) {
        return b.token == frame.token;
    });
    if (it == s.buffers.end()) return invalid("release frame");
    it->token = 0;
    frame = {};
    --s.stats.outstanding_frames;
    if (!s.running) return IRIS_OK;
    const auto handle = it->handle;
    if (!keep_lock) {
        ++s.requeues;
        lock.unlock();
    }
    auto &p = *s.device->producer;
    const auto status = api("DSQueueBuffer", [&] {
        return p.error(p.DSQueueBuffer(s.handle, handle), "DSQueueBuffer");
    });
    if (!keep_lock) {
        lock.lock();
        --s.requeues;
        s.idle.notify_all();
    }
    if (status < 0) {
        ++s.stats.requeue_failures;
        s.running = false;
        s.stopping = true;
    }
    return status;
}

} // namespace

iris_status_t iris_stream_release_frame(iris_stream_t *s, iris_frame_t *frame)
{
    return api("release frame", [&]() -> iris_status_t {
        if (!s || !frame || !frame->token) return invalid("release frame");
        std::unique_lock lock(s->mutex);
        return release_frame(*s, *frame, lock, false);
    });
}
iris_status_t iris_stream_poll(iris_stream_t *s, iris_frame_handler_t handler,
                                void *data, size_t limit, size_t *delivered)
{
    return api("poll", [&]() -> iris_status_t {
        if (!delivered) return invalid("poll");
        *delivered = 0;
        if (!s || !handler || !limit) return invalid("poll");
        // Keep the admission lock through the bounded batch. An admitted
        // nonblocking reader never has to reacquire a consumer lock.
        std::unique_lock lock(s->mutex, std::try_to_lock);
        if (!lock || s->reader || s->partial.buffer) return fail(IRIS_BUSY, "poll", "stream reader busy");
        if (!s->running) return IRIS_STOPPED;
        while (*delivered < limit) {
            iris_frame_t frame{};
            auto status = receive(*s, 0, frame, lock);
            if (status == IRIS_NO_FRAME) return IRIS_OK;
            if (status != IRIS_OK) return status;
            ++*delivered;
            const auto handled = api("frame handler", [&] {
                handler(data, &frame);
                return IRIS_OK;
            });
            if (handled < 0) {
                const auto error = last;
                release_frame(*s, frame, lock, true);
                last = error;
                return handled;
            }
            status = release_frame(*s, frame, lock, true);
            if (status != IRIS_OK) return status;
        }
        return IRIS_OK;
    });
}
namespace {

bool progressive_format(uint64_t format)
{
    return format == 0x01100003 || format == 0x01100005 ||
           format == 0x01100025 || format == 0x01100007;
}

bool same_target(const iris_row_target_t &a, const iris_row_target_t &b)
{
    return a.data == b.data && a.capacity == b.capacity && a.width == b.width &&
           a.height == b.height && a.pixel_format == b.pixel_format;
}

bool separate_image(const void *base, size_t bytes, const iris_row_target_t &target)
{
    const auto src = reinterpret_cast<uintptr_t>(base);
    const auto dst = reinterpret_cast<uintptr_t>(target.data);
    return bytes <= UINTPTR_MAX - src && (src + bytes <= dst || dst + bytes <= src);
}

bool active_image(iris_stream &s, const Buffer &buffer, const iris_row_target_t &target,
                  size_t bytes, void *&base)
{
    const auto h = buffer.handle;
    size_t width = 0, height = 0, offset = 0, padding = 0, capacity = 0;
    uint64_t format = 0;
    int32_t endian = 0;
    bool8_t composite = 0, chunks = 0;
    size_t payload_type = 0;
    return buffer.capacity == bytes &&
           (!buffer_info(s, h, BUFFER_INFO_PIXEL_ENDIANNESS, endian) || endian != PIXELENDIANNESS_BIG) &&
           (!buffer_info(s, h, BUFFER_INFO_PAYLOADTYPE, payload_type) || payload_type == PAYLOAD_TYPE_IMAGE) &&
           (!buffer_info(s, h, BUFFER_INFO_IS_COMPOSITE, composite) || !composite) &&
           (!buffer_info(s, h, BUFFER_INFO_CONTAINS_CHUNKDATA, chunks) || !chunks) &&
           buffer_info(s, h, BUFFER_INFO_WIDTH, width) && width == target.width &&
           buffer_info(s, h, BUFFER_INFO_HEIGHT, height) && height == target.height &&
           buffer_info(s, h, BUFFER_INFO_PIXELFORMAT, format) && format == target.pixel_format &&
           buffer_info(s, h, BUFFER_INFO_IMAGEOFFSET, offset) && !offset &&
           buffer_info(s, h, BUFFER_INFO_XPADDING, padding) && !padding &&
           buffer_info(s, h, BUFFER_INFO_SIZE, capacity) && capacity >= bytes &&
           buffer_info(s, h, BUFFER_INFO_BASE, base) && base &&
           (!buffer.caller_data || base == buffer.caller_data) &&
           (!buffer.storage || base == buffer.storage) && separate_image(base, bytes, target);
}

void copy_rows(const iris_row_target_t &target, size_t row_bytes, const void *base,
               uint32_t first, uint32_t end, bool complete, iris_row_update_t &out)
{
    auto *dst = static_cast<unsigned char *>(target.data);
    const auto *src = static_cast<const unsigned char *>(base);
    if (end > first) {
        const size_t offset = first * row_bytes;
        std::memcpy(dst + offset, src + offset, (end - first) * row_bytes);
    }
    out = {first, end - first, complete ? 1u : 0u};
}

iris_status_t progress_rows(iris_stream &s, const iris_row_target_t &target,
                            size_t row_bytes, iris_row_update_t &out)
{
    Buffer *selected = s.partial.buffer;
    size_t filled = 0;
    for (auto &candidate : s.buffers) {
        if ((selected && selected != &candidate) || candidate.token) continue;
        bool8_t active = 0;
        if (!buffer_info(s, candidate.handle, BUFFER_INFO_IS_ACQUIRING, active))
            return fail(IRIS_NOT_SUPPORTED, "poll rows", "acquiring-buffer flag is unavailable");
        if (!active) continue;
        if (!buffer_info(s, candidate.handle, BUFFER_INFO_SIZE_FILLED, filled))
            return fail(IRIS_NOT_SUPPORTED, "poll rows", "active fill count is unavailable");
        const size_t frame_bytes = row_bytes * target.height;
        if (filled > frame_bytes || filled / row_bytes < s.partial.rows)
            return fail(IRIS_IO_ERROR, "poll rows", "invalid acquiring-buffer fill count");
        const auto rows = static_cast<uint32_t>(filled / row_bytes);
        if (rows == s.partial.rows) return IRIS_NO_FRAME;
        void *base = nullptr;
        const bool layout_ok = selected || active_image(s, candidate, target, frame_bytes, base);
        if (!buffer_info(s, candidate.handle, BUFFER_INFO_IS_ACQUIRING, active) || !active)
            return IRIS_NO_FRAME; // Let the matching completion validate the image.
        if (!layout_ok)
            return fail(IRIS_NOT_SUPPORTED, "poll rows", "active image layout is unavailable or incompatible");
        // The producer contract for this mode guarantees the prefix stays visible
        // and stable until the completion is consumed and the buffer requeued.
        const void *source = selected ? s.partial.base : base;
        copy_rows(target, row_bytes, source, s.partial.rows, rows, false, out);
        s.partial = {&candidate, target, source, rows};
        return IRIS_OK;
    }
    return IRIS_NO_FRAME;
}

} // namespace

iris_status_t iris_stream_poll_rows(iris_stream_t *s, const iris_row_target_t *target,
                                    iris_row_update_t *out)
{
    return api("poll rows", [&]() -> iris_status_t {
        if (!out) return invalid("poll rows");
        *out = {};
        if (!s || !target || !target->data || !target->width || !target->height ||
            !progressive_format(target->pixel_format)) return invalid("poll rows");
        const auto stride = row_bytes(target->pixel_format, target->width);
        if (!stride || target->height > target->capacity / stride ||
            target->capacity > UINTPTR_MAX - reinterpret_cast<uintptr_t>(target->data))
            return fail(IRIS_BUFFER_TOO_SMALL, "poll rows", "invalid destination size");
        std::unique_lock lock(s->mutex, std::try_to_lock);
        if (!lock || s->reader || s->stats.outstanding_frames)
            return fail(IRIS_BUSY, "poll rows", "stream reader busy");
        if (!s->running) return IRIS_STOPPED;
        if (s->partial.buffer && !same_target(s->partial.target, *target))
            return fail(IRIS_INVALID_STATE, "poll rows", "destination changed before completion");
        iris_frame_t frame{};
        Buffer *source = nullptr;
        auto status = receive(*s, 0, frame, lock, &source);
        if (status == IRIS_NO_FRAME) return progress_rows(*s, *target, stride, *out);
        if (status != IRIS_OK) return status;
        const bool valid = (!s->partial.buffer ||
                            (s->partial.buffer == source && s->partial.base == frame.data)) &&
            frame.status == IRIS_FRAME_COMPLETE && frame.data &&
            frame.width == target->width && frame.height == target->height &&
            frame.pixel_format == target->pixel_format && frame.stride_bytes == stride &&
            frame.image_size == stride * target->height && frame.payload.image_offset == 0 &&
            !frame.payload.contains_chunk_data && separate_image(frame.data, frame.image_size, *target) &&
            (!(frame.valid & IRIS_FRAME_HAS_PIXEL_ENDIANNESS) ||
             frame.pixel_endianness != PIXELENDIANNESS_BIG);
        if (valid) copy_rows(*target, stride, frame.data, s->partial.rows, target->height, true, *out);
        s->partial = {};
        status = release_frame(*s, frame, lock, true);
        if (status != IRIS_OK || !valid) *out = {};
        if (status != IRIS_OK) return status;
        return valid ? IRIS_OK : fail(IRIS_IO_ERROR, "poll rows", "invalid or mismatched completion");
    });
}

iris_status_t iris_stream_stats(iris_stream_t *s, iris_stream_stats_t *stats)
{
    return api("stream stats", [&]() -> iris_status_t {
        if (!s || !stats) return invalid("stream stats");
        std::lock_guard lock(s->mutex);
        *stats = s->stats;
        return IRIS_OK;
    });
}
