#include "node_map.hpp"

#include "event_snapshot.hpp"
#include "gentl_info.hpp"

#include <algorithm>
#include <charconv>
#include <time.h>

using namespace GenTL;
using namespace iris;

namespace {

constexpr size_t max_event_bytes = 1024 * 1024;
constexpr size_t initial_event_bytes = 256;

bool absent(GC_ERROR status)
{
    return status == GC_ERR_NOT_AVAILABLE || status == GC_ERR_NOT_IMPLEMENTED || status == GC_ERR_NO_DATA;
}

size_t event_buffer_size(Producer &p, EVENT_HANDLE event, EVENT_INFO_CMD command)
{
    size_t bytes = initial_event_bytes;
    if (p.EventGetInfo)
        get_info(bytes, [&](auto *type, auto *value, auto *size) {
            return p.EventGetInfo(event, command, type, value, size);
        });
    require(bytes <= max_event_bytes, IRIS_OUT_OF_RANGE, "EventGetInfo", "event buffer exceeds 1 MiB");
    return std::max<size_t>(1, bytes);
}

void grow_event_buffer(std::vector<uint8_t> &buffer, size_t needed, const char *operation)
{
    if (needed <= buffer.size()) needed = std::min(max_event_bytes, buffer.size() * 2);
    require(needed > buffer.size() && needed <= max_event_bytes, IRIS_OUT_OF_RANGE,
            operation, "event buffer exceeds 1 MiB");
    buffer.resize(needed);
}

void read_event_id(Producer &p, NodeMapEvents &sub, size_t received, iris_node_map_event_t &event)
{
    INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
    size_t size = sizeof event.numeric_id;
    const auto result = p.EventGetDataInfo(sub.handle, sub.envelope.data(), received,
                                           EVENT_DATA_NUMID, &type, &event.numeric_id, &size);
    if (result == GC_ERR_SUCCESS) {
        require(type == INFO_DATATYPE_UINT64 && size == sizeof event.numeric_id,
                IRIS_PRODUCER_ERROR, "EventGetDataInfo", "invalid numeric event ID");
        event.has_numeric_id = 1;
        auto *digits = event.id + 1;
        auto converted = std::to_chars(digits, digits + 16, event.numeric_id, 16);
        const auto length = converted.ptr - digits;
        if (length % 2) event.id[0] = '0';
        else std::memmove(event.id, digits, length);
        event.id[length + length % 2] = '\0';
        return;
    }
    if (!absent(result)) p.check(result, "EventGetDataInfo");
    event.numeric_id = 0;
    type = INFO_DATATYPE_UNKNOWN;
    size = sizeof event.id;
    p.check(p.EventGetDataInfo(sub.handle, sub.envelope.data(), received,
                               EVENT_DATA_ID, &type, event.id, &size), "EventGetDataInfo");
    require(type == INFO_DATATYPE_STRING && size >= 3 && size <= sizeof event.id &&
            event.id[size - 1] == '\0' && size % 2 == 1,
            IRIS_PRODUCER_ERROR, "EventGetDataInfo", "invalid hexadecimal event ID");
    const auto end = event.id + size - 1;
    const auto valid_hex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    require(std::all_of(event.id, end, valid_hex), IRIS_PRODUCER_ERROR,
            "EventGetDataInfo", "invalid hexadecimal event ID");
    const auto parsed = std::from_chars(event.id, end, event.numeric_id, 16);
    event.has_numeric_id = parsed.ec == std::errc{};
}

size_t read_event_data(Producer &p, NodeMapEvents &sub, size_t received)
{
    for (;;) {
        INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
        size_t size = sub.data.size();
        const auto result = p.EventGetDataInfo(sub.handle, sub.envelope.data(), received,
                                               EVENT_DATA_VALUE, &type, sub.data.data(), &size);
        if (absent(result)) return 0;
        if (result == GC_ERR_BUFFER_TOO_SMALL) {
            grow_event_buffer(sub.data, size, "EventGetDataInfo");
            continue;
        }
        p.check(result, "EventGetDataInfo");
        require(type == INFO_DATATYPE_BUFFER && size <= sub.data.size(), IRIS_PRODUCER_ERROR,
                "EventGetDataInfo", "invalid event payload");
        return size;
    }
}

void unregister_events(iris_node_map &map)
{
    auto &p = map.port.producer;
    p.check(p.GCUnregisterEvent(map.event_source, map.event_type), "GCUnregisterEvent");
    map.events.reset();
}

} // namespace

iris_status_t iris::receive_event_envelope(Producer &p, EVENT_HANDLE event,
                                            std::vector<uint8_t> &data, size_t &size, size_t *processed)
{
    for (;;) {
        size = data.size();
        const auto result = p.EventGetData(event, data.data(), &size, 0);
        if (result == GC_ERR_TIMEOUT) return IRIS_NO_EVENT;
        if (result != GC_ERR_BUFFER_TOO_SMALL) {
            p.check(result, "EventGetData");
            if (processed) ++*processed;
            require(size && size <= data.size(), IRIS_PRODUCER_ERROR,
                    "EventGetData", "invalid event envelope size");
            return IRIS_OK;
        }
        if (size > max_event_bytes || data.size() == max_event_bytes) {
            // GenTL retains the event after BUFFER_TOO_SMALL. Discard that queue head.
            size_t discarded = 0;
            p.check(p.EventGetData(event, nullptr, &discarded, 0), "EventGetData");
            if (processed) ++*processed;
            return fail(IRIS_OUT_OF_RANGE, "EventGetData", "discarded event envelope larger than 1 MiB");
        }
        grow_event_buffer(data, size, "EventGetData");
    }
}

iris_status_t iris_node_map_enable_events(iris_node_map_t *map, uint32_t enabled)
{
    constexpr auto operation = "enable node map events";
    if (!map || enabled > 1) return invalid(operation);
    return api(operation, [&]() -> iris_status_t {
        std::lock_guard lock(*map->mutex);
        if (!enabled) {
            if (map->events) unregister_events(*map);
            return IRIS_OK;
        }
        if (map->events) {
            require(map->events->ready, IRIS_INVALID_STATE, operation, "unsubscribe before retrying");
            return IRIS_OK;
        }
        auto &p = map->port.producer;
        require(p.EventGetDataInfo, IRIS_NOT_SUPPORTED, operation, "producer lacks EventGetDataInfo");
        auto sub = std::make_unique<NodeMapEvents>(map->nodes._Ptr);
        const auto status = api(operation, [&] {
            p.check(p.GCRegisterEvent(map->event_source, map->event_type, &sub->handle), "GCRegisterEvent");
            require(sub->handle, IRIS_PRODUCER_ERROR, "GCRegisterEvent", "missing event handle");
            sub->envelope.resize(event_buffer_size(p, sub->handle, EVENT_SIZE_MAX));
            sub->data.resize(event_buffer_size(p, sub->handle, EVENT_INFO_DATA_SIZE_MAX));
            sub->ready = true;
            return IRIS_OK;
        });
        if (sub->handle) map->events = std::move(sub);
        if (status < 0 && map->events) {
            const auto diagnostic = last;
            api("GCUnregisterEvent", [&] { unregister_events(*map); return IRIS_OK; });
            last = diagnostic;
        }
        return status;
    });
}

namespace {

template<class F>
iris_status_t try_node_map_event(iris_node_map_t *map, bool timestamp, F deliver)
{
    constexpr auto operation = "try node map event";
    if (!map) return invalid(operation);
    return api(operation, [&]() -> iris_status_t {
        std::unique_lock lock(*map->mutex, std::try_to_lock);
        if (!lock) return fail(IRIS_BUSY, operation, "feature lock busy");
        require(map->events && map->events->ready, IRIS_INVALID_STATE, operation, "events are disabled");
        auto &sub = *map->events;
        auto &p = map->port.producer;
        size_t received;
        const auto status = receive_event_envelope(p, sub.handle, sub.envelope, received);
        if (status != IRIS_OK) return status;

        iris_event_snapshot_info_t info{};
        timespec now{};
        if (timestamp && clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
            info.host_timestamp_ns = uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
            info.valid |= IRIS_EVENT_HAS_HOST_TIMESTAMP_NS;
        }
        auto &event = info.event;
        read_event_id(p, sub, received, event);
        event.data_size = read_event_data(p, sub, received);
        if (event.has_numeric_id)
            sub.adapter.DeliverMessage(sub.data.data(), event.data_size, event.numeric_id);
        else
            sub.adapter.DeliverMessage(sub.data.data(), event.data_size, GenICam::gcstring(event.id));
        info.data = event.data_size ? sub.data.data() : nullptr;
        deliver(map->nodes, info);
        return IRIS_OK;
    });
}

} // namespace

iris_status_t iris_node_map_try_get_event(iris_node_map_t *map, iris_node_map_event_t *out)
{
    if (!out) return invalid("try node map event");
    *out = {};
    return try_node_map_event(map, false, [&](auto &, const auto &info) { *out = info.event; });
}

iris_status_t iris_node_map_try_get_event_snapshot(iris_node_map_t *map, iris_event_snapshot_t **out)
{
    if (!out) return invalid("try node map event");
    *out = nullptr;
    return try_node_map_event(map, true, [&](auto &nodes, const auto &info) {
        *out = capture_event_snapshot(*nodes._Ptr, info).release();
    });
}

iris_status_t iris::disable_node_map_events(iris_node_map *map)
{
    if (!map) return IRIS_OK;
    if (map->events) {
        const auto status = iris_node_map_enable_events(map, 0);
        if (status != IRIS_OK) return status;
    }
    return map->invalidation ? iris_node_map_enable_invalidation(map, 0) : IRIS_OK;
}
