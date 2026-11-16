#include "stream.hpp"

#include "device.hpp"
#include "error.hpp"
#include "gentl_info.hpp"
#include "node_map.hpp"
#include "producer.hpp"

#include <algorithm>
#include <cstdlib>

using namespace GenTL;
using namespace iris;

namespace {

bool stream_info(iris_stream &s, STREAM_INFO_CMD cmd, size_t &out)
{
    return get_info(out, [&](auto *type, auto *value, auto *size) {
        return s.device->producer->DSGetInfo(s.handle, cmd, type, value, size);
    });
}

void buffer_requirements(iris_stream &s, iris_buffer_requirements_t &requirements)
{
    auto &p = *s.device->producer;
    requirements = {0, 1, 1};
    auto &payload = requirements.payload_size;
    bool8_t defines = 0;
    get_info(defines, [&](auto *type, auto *value, auto *size) {
        return p.DSGetInfo(s.handle, STREAM_INFO_DEFINES_PAYLOADSIZE, type, value, size);
    });
    if (defines) stream_info(s, STREAM_INFO_PAYLOAD_SIZE, payload);
    if (!payload && s.remote_map) {
        int64_t value = 0;
        if (iris_feature_get_integer(s.remote_map, "PayloadSize", &value) == IRIS_OK && value > 0)
            payload = value;
    }
    if (!payload) stream_info(s, STREAM_INFO_PAYLOAD_SIZE, payload);
    require(payload, IRIS_NOT_SUPPORTED, "buffer requirements", "payload size unavailable");
    stream_info(s, STREAM_INFO_BUF_ANNOUNCE_MIN, requirements.minimum_buffer_count);
    stream_info(s, STREAM_INFO_BUF_ALIGNMENT, requirements.alignment);
    requirements.minimum_buffer_count = std::max<size_t>(1, requirements.minimum_buffer_count);
    requirements.alignment = std::max<size_t>(1, requirements.alignment);
}

void validate_buffers(const iris_buffer_t *buffers, size_t count,
                      const iris_buffer_requirements_t &requirements)
{
    std::vector<std::pair<uintptr_t, uintptr_t>> ranges;
    ranges.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &buffer = buffers[i];
        const auto base = reinterpret_cast<uintptr_t>(buffer.data);
        require(base && base % requirements.alignment == 0 && buffer.size <= UINTPTR_MAX - base,
                IRIS_INVALID_ARGUMENT, "stream start", "invalid caller buffer address or alignment");
        require(buffer.size >= requirements.payload_size, IRIS_BUFFER_TOO_SMALL,
                "stream start", "caller buffer smaller than payload");
        ranges.emplace_back(base, base + buffer.size);
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i)
        require(ranges[i - 1].second <= ranges[i].first, IRIS_INVALID_ARGUMENT,
                "stream start", "overlapping caller buffers");
}

void announce(iris_stream &s, Buffer &buffer, const iris_buffer_t *caller,
              const iris_buffer_requirements_t &requirements)
{
    auto &p = *s.device->producer;
    if (caller) {
        buffer.capacity = caller->size;
        buffer.caller_data = caller->data;
        buffer.user_data = caller->user_data;
        p.check(p.DSAnnounceBuffer(s.handle, caller->data, caller->size, &buffer, &buffer.handle),
                "DSAnnounceBuffer");
        return;
    }
    buffer.capacity = requirements.payload_size;
    const auto result = p.DSAllocAndAnnounceBuffer(s.handle, buffer.capacity, &buffer, &buffer.handle);
    if (buffer.handle || (result != GC_ERR_NOT_IMPLEMENTED && result != GC_ERR_NOT_AVAILABLE)) {
        p.check(result, "DSAllocAndAnnounceBuffer");
        return;
    }
    const auto required = requirements.alignment;
    require((required & (required - 1)) == 0, IRIS_NOT_SUPPORTED, "stream start", "unsupported buffer alignment");
    const auto align = std::max(required, cache_line_size);
    require(posix_memalign(&buffer.storage, align, buffer.capacity) == 0,
            IRIS_NO_MEMORY, "stream start", "buffer allocation failed");
    p.check(p.DSAnnounceBuffer(s.handle, buffer.storage, buffer.capacity, &buffer, &buffer.handle),
            "DSAnnounceBuffer");
}

iris_status_t stop(iris_stream &s)
{
    auto &p = *s.device->producer;
    iris_status_t result = IRIS_OK;
    Error first;
    auto record = [&](iris_status_t status) {
        if (status < 0 && result == IRIS_OK) { result = status; first = last; }
        return status == IRIS_OK;
    };
    auto producer_call = [&](const char *operation, auto call) {
        return api(operation, [&] { return p.error(call(), operation); });
    };
    {
        std::unique_lock lock(s.mutex);
        s.running = false;
        s.stopping = true;
        s.partial = {};
        if (s.reader && s.event) {
            lock.unlock();
            record(producer_call("EventKill", [&] {
                const auto rc = p.EventKill(s.event);
                return rc == GC_ERR_NOT_IMPLEMENTED || rc == GC_ERR_NOT_AVAILABLE
                    ? GC_ERR_SUCCESS : rc;
            }));
            lock.lock();
        }
        s.idle.wait(lock, [&] { return !s.reader && !s.requeues; });
    }
    if (s.remote && record(iris_feature_execute(s.remote_map, "AcquisitionStop"))) s.remote = false;
    if (s.host && record(producer_call("DSStopAcquisition", [&] { return p.DSStopAcquisition(s.handle, ACQ_STOP_FLAGS_DEFAULT); }))) s.host = false;
    if (!s.host) {
        if (s.event_registered && record(producer_call("GCUnregisterEvent", [&] {
                return p.GCUnregisterEvent(s.handle, EVENT_NEW_BUFFER);
            }))) {
            s.event = nullptr;
            s.event_registered = false;
        }
        if (s.flush && record(producer_call("DSFlushQueue", [&] { return p.DSFlushQueue(s.handle, ACQ_QUEUE_ALL_DISCARD); })))
            s.flush = false;
    }
    {
        std::unique_lock lock(s.mutex);
        if (!s.host && !s.event_registered && !s.flush) {
            for (auto &buffer : s.buffers) {
                if (buffer.token) continue;
                if (buffer.handle && !record(producer_call("DSRevokeBuffer", [&] { return p.DSRevokeBuffer(s.handle, buffer.handle, nullptr, nullptr); })))
                    continue;
                buffer.handle = nullptr;
                std::free(buffer.storage);
                buffer.storage = nullptr;
            }
        }
        if (!s.stats.outstanding_frames &&
            std::none_of(s.buffers.begin(), s.buffers.end(), [](const Buffer &b) { return b.handle; })) {
            s.buffers.clear();
            if (s.locked && !s.host && !s.event_registered && !s.flush) {
                // Some ports reject configuration writes while the pool is announced.
                lock.unlock();
                if (record(iris_feature_set_integer(s.remote_map, "TLParamsLocked", 0))) s.locked = false;
                lock.lock();
            }
            if (!s.remote && !s.host && !s.event_registered && !s.flush && !s.locked) {
                s.stopping = false;
                if (s.device->active == &s) s.device->active = nullptr;
            }
        }
    }
    if (result < 0) last = first;
    return result;
}

bool can_retire(iris_stream &s)
{
    std::lock_guard lock(s.mutex);
    return s.stopping && !s.host && !s.remote && !s.flush &&
           !s.event_registered && !s.stats.outstanding_frames;
}

} // namespace

iris_status_t iris::close_failed_stream(iris_device &device)
{
    if (!device.failed_stream) return IRIS_OK;
    auto &p = *device.producer;
    const auto status = api("DSClose", [&] {
        return p.error(p.DSClose(device.failed_stream), "DSClose");
    });
    if (status == IRIS_OK) device.failed_stream = nullptr;
    return status;
}

iris_status_t iris_stream_open(iris_device_t *d, size_t index, iris_stream_t **out)
{
    if (!out) return invalid("stream open");
    *out = nullptr;
    if (!d || index > UINT32_MAX) return invalid("stream open");
    return api("stream open", [&]() -> iris_status_t {
        require(!d->closing, IRIS_INVALID_STATE, "stream open", "device close pending");
        const auto recovered = close_failed_stream(*d);
        if (recovered != IRIS_OK) return recovered;
        auto &p = *d->producer;
        auto id = text_query(p, "DevGetDataStreamID", [&](char *s, size_t *size) {
            return p.DevGetDataStreamID(d->handle, static_cast<uint32_t>(index), s, size);
        });
        auto stream = std::make_unique<iris_stream>();
        const auto status = api("DevOpenDataStream", [&] {
            return p.error(p.DevOpenDataStream(d->handle, id.c_str(), &stream->handle), "DevOpenDataStream");
        });
        if (status != IRIS_OK) {
            const auto diagnostic = last;
            d->failed_stream = stream->handle;
            close_failed_stream(*d);
            last = diagnostic;
            return status;
        }
        stream->device = d;
        ++d->streams;
        *out = stream.release();
        return IRIS_OK;
    });
}

iris_status_t iris_stream_start(iris_stream_t *s, const iris_start_options_t *options)
{
    if (!s) return invalid("stream start");
    const auto control = options ? options->acquisition_control : IRIS_ACQUISITION_REMOTE_AND_STREAM;
    if (control != IRIS_ACQUISITION_REMOTE_AND_STREAM && control != IRIS_ACQUISITION_STREAM_ONLY)
        return invalid("stream start");
    const auto *buffers = options ? options->buffers : nullptr;
    if (buffers && !options->buffer_count) return invalid("stream start");
    return api("stream start", [&]() -> iris_status_t {
        if (can_retire(*s)) {
            const auto retired = stop(*s);
            if (retired != IRIS_OK) return retired;
        }
        auto &p = *s->device->producer;
        {
            std::lock_guard lock(s->mutex);
            require(!s->device->active && !s->stopping && !s->stats.outstanding_frames,
                    IRIS_BUSY, "stream start", "device acquisition or cleanup active");
        }
        const auto rc = iris_device_node_map(s->device, IRIS_REMOTE_DEVICE, &s->remote_map);
        if (rc < 0 && control == IRIS_ACQUISITION_REMOTE_AND_STREAM) return rc;
        iris_buffer_requirements_t requirements;
        buffer_requirements(*s, requirements);
        const auto minimum = requirements.minimum_buffer_count;
        size_t count = options ? options->buffer_count : 0;
        if (!count) count = std::max<size_t>(8, minimum);
        require(count >= minimum, IRIS_OUT_OF_RANGE, "stream start", "buffer count below producer minimum");
        if (buffers) validate_buffers(buffers, count, requirements);
        s->buffers.resize(count);
        s->device->active = s;
        const auto started = api("stream start", [&]() -> iris_status_t {
            iris_feature_info_t info{};
            if (s->remote_map &&
                iris_feature_info(s->remote_map, "TLParamsLocked", &info) == IRIS_OK &&
                (info.access == IRIS_FEATURE_READ_WRITE || info.access == IRIS_FEATURE_WRITE_ONLY)) {
                s->locked = true;
                const auto result = iris_feature_set_integer(s->remote_map, "TLParamsLocked", 1);
                if (result < 0) return result;
            }
            const auto registered = api("GCRegisterEvent", [&] {
                return p.error(p.GCRegisterEvent(s->handle, EVENT_NEW_BUFFER, &s->event), "GCRegisterEvent");
            });
            s->event_registered = s->event || registered == IRIS_OK;
            if (registered < 0) return registered;
            require(s->event, IRIS_PRODUCER_ERROR, "GCRegisterEvent", "missing buffer event handle");
            for (size_t i = 0; i < count; ++i) {
                auto &buffer = s->buffers[i];
                announce(*s, buffer, buffers ? &buffers[i] : nullptr, requirements);
                require(buffer.handle, IRIS_PRODUCER_ERROR, "announce buffer", "missing buffer handle");
                s->flush = true;
                p.check(p.DSQueueBuffer(s->handle, buffer.handle), "DSQueueBuffer");
            }
            // A failed start may have armed the producer before returning.
            s->host = true;
            const auto started = api("DSStartAcquisition", [&] {
                return p.error(p.DSStartAcquisition(s->handle, ACQ_START_FLAGS_DEFAULT, GENTL_INFINITE),
                               "DSStartAcquisition");
            });
            if (started < 0) {
                bool8_t grabbing = 0;
                const auto diagnostic = last;
                const auto queried = api("DSGetInfo", [&] {
                    if (get_info(grabbing, [&](auto *type, auto *value, auto *size) {
                            return p.DSGetInfo(s->handle, STREAM_INFO_IS_GRABBING, type, value, size);
                        }) && !grabbing) s->host = false;
                    return IRIS_OK;
                });
                if (queried < 0) s->host = true;
                last = diagnostic;
                return started;
            }
            if (control == IRIS_ACQUISITION_REMOTE_AND_STREAM) {
                s->remote = true;
                const auto result = iris_feature_execute(s->remote_map, "AcquisitionStart");
                if (result < 0) return result;
            }
            std::lock_guard lock(s->mutex);
            s->running = true;
            return IRIS_OK;
        });
        if (started < 0) {
            const auto saved = last;
            stop(*s);
            last = saved;
        }
        return started;
    });
}

iris_status_t iris_stream_buffer_requirements(iris_stream_t *s, iris_buffer_requirements_t *out)
{
    if (!s || !out) return invalid("buffer requirements");
    return api("buffer requirements", [&] {
        {
            std::lock_guard lock(s->mutex);
            require(!s->running && !s->stopping, IRIS_BUSY, "buffer requirements", "stream is active");
        }
        iris_device_node_map(s->device, IRIS_REMOTE_DEVICE, &s->remote_map);
        iris_buffer_requirements_t requirements;
        buffer_requirements(*s, requirements);
        *out = requirements;
        return IRIS_OK;
    });
}

iris_status_t iris_stream_stop(iris_stream_t *s)
{
    if (!s) return invalid("stream stop");
    return api("stream stop", [&] { return stop(*s); });
}
iris_status_t iris_stream_close(iris_stream_t **out)
{
    return api("stream close", [&]() -> iris_status_t {
        if (!out || !*out) return invalid("stream close");
        auto &s = **out;
        if (can_retire(s)) {
            const auto status = iris_stream_stop(&s);
            if (status != IRIS_OK) return status;
        }
        {
            std::lock_guard lock(s.mutex);
            if (s.running || s.stopping || s.stats.outstanding_frames)
                return fail(IRIS_BUSY, "stream close", "stop and release outstanding frames first");
        }
        const auto events = disable_node_map_events(s.map.get());
        if (events != IRIS_OK) return events;
        auto &p = *s.device->producer;
        const auto status = p.error(p.DSClose(s.handle), "DSClose");
        if (status == IRIS_OK) {
            --s.device->streams;
            delete *out;
            *out = nullptr;
        }
        return status;
    });
}
