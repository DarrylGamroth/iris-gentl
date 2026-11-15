#include "node_map.hpp"

#include "device.hpp"
#include "error.hpp"
#include "gentl_info.hpp"
#include "producer.hpp"
#include "stream.hpp"

#include <vector>

namespace iris {

Port::Port(Producer &p, GenTL::PORT_HANDLE port, bool writable)
    : producer(p), handle(port), access(writable ? GenApi::RW : GenApi::RO) {}

GenApi::EAccessMode Port::GetAccessMode() const
{
    const auto flag = [&](GenTL::PORT_INFO_CMD cmd, bool fallback) {
        bool8_t value = fallback;
        get_info(value, [&](auto *type, auto *out, auto *size) {
            return producer.GCGetPortInfo(handle, cmd, type, out, size);
        });
        return value != 0;
    };
    if (flag(GenTL::PORT_INFO_ACCESS_NI, false)) return GenApi::NI;
    if (flag(GenTL::PORT_INFO_ACCESS_NA, false)) return GenApi::NA;
    const bool read = flag(GenTL::PORT_INFO_ACCESS_READ, true);
    const bool write = access == GenApi::RW && flag(GenTL::PORT_INFO_ACCESS_WRITE, true);
    if (read) return write ? GenApi::RW : GenApi::RO;
    return write ? GenApi::WO : GenApi::NA;
}
void Port::Read(void *data, int64_t address, int64_t length)
{
    transfer(address, length, [&](size_t &n) {
        return producer.GCReadPort(handle, address, data, &n);
    }, "GCReadPort");
}
void Port::Write(const void *data, int64_t address, int64_t length)
{
    if (stacked_written) {
        require(address >= 0 && length >= 0, IRIS_OUT_OF_RANGE, "GCWritePortStacked", "invalid register extent");
        GenApi::PORT_REGISTER_STACK_ENTRY entry{static_cast<uint64_t>(address),
                                                const_cast<void *>(data), static_cast<size_t>(length)};
        Write(&entry, 1);
        return;
    }
    transfer(address, length, [&](size_t &n) {
        return producer.GCWritePort(handle, address, data, &n);
    }, "GCWritePort");
}
template<class F>
void Port::transfer(int64_t address, int64_t length, F f, const char *op)
{
    require(address >= 0 && length >= 0 && length <= INT64_MAX - address,
            IRIS_OUT_OF_RANGE, op, "invalid register extent");
    size_t size = length;
    producer.check(f(size), op);
    require(size == static_cast<size_t>(length), IRIS_IO_ERROR, op, "short register transfer");
}

} // namespace iris

using namespace GenTL;
using namespace iris;

namespace {

alignas(cache_line_size) std::mutex feature_mutex;

} // namespace

std::unique_ptr<iris_node_map> iris::load_remote_node_map(iris_device &d, const char *xml_path)
{
    auto &p = *d.producer;
    PORT_HANDLE port = nullptr;
    p.check(p.DevGetPort(d.handle, &port), "DevGetPort");
    auto map = load_node_map(p, port, d.writable, feature_mutex, xml_path);
    map->event_source = d.handle;
    map->event_type = EVENT_REMOTE_DEVICE;
    return map;
}

iris_status_t iris_device_node_map(iris_device_t *d, iris_module_t module, iris_node_map_t **out)
{
    if (!out) return invalid("device node map");
    *out = nullptr;
    if (!d || module < IRIS_SYSTEM_MODULE || module > IRIS_REMOTE_DEVICE)
        return invalid("device node map");
    return api("device node map", [&] {
        std::lock_guard registry_lock(registry_mutex);
        require(!d->closing, IRIS_INVALID_STATE, "device node map", "device close pending");
        auto &p = *d->producer;
        PORT_HANDLE port = nullptr;
        std::unique_ptr<iris_node_map> *map = nullptr;
        bool writable = d->writable;
        switch (module) {
        case IRIS_SYSTEM_MODULE:
            port = p.system;
            map = &p.map;
            writable = true;
            break;
        case IRIS_INTERFACE_MODULE:
            port = d->iface;
            map = &p.interfaces.at(d->interface).map;
            writable = true;
            break;
        case IRIS_DEVICE_MODULE:
            port = d->handle;
            map = &d->maps[0];
            break;
        case IRIS_REMOTE_DEVICE:
            map = &d->maps[1];
            if (!*map) *map = load_remote_node_map(*d);
            break;
        }
        if (!*map) {
            *map = load_node_map(p, port, writable, feature_mutex);
            (*map)->event_source = port;
        }
        *out = map->get();
        return IRIS_OK;
    });
}
iris_status_t iris_stream_node_map(iris_stream_t *s, iris_node_map_t **out)
{
    if (!out) return invalid("stream node map");
    *out = nullptr;
    if (!s) return invalid("stream node map");
    return api("stream node map", [&] {
        if (!s->map) {
            s->map = load_node_map(*s->device->producer, s->handle, s->device->writable, feature_mutex);
            s->map->event_source = s->handle;
        }
        *out = s->map.get();
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_invalidate(iris_node_map_t *map)
{
    if (!map) return invalid("invalidate node map");
    return api("invalidate node map", [&] {
        std::lock_guard lock(*map->mutex);
        map->nodes._InvalidateNodes();
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_enable_invalidation(iris_node_map_t *map, uint32_t enabled)
{
    if (!map || enabled > 1) return invalid("enable invalidation");
    return api("enable invalidation", [&]() -> iris_status_t {
        std::lock_guard lock(*map->mutex);
        auto &p = map->port.producer;
        const auto unregister = [&] {
            p.check(p.GCUnregisterEvent(map->invalidation_source, EVENT_FEATURE_INVALIDATE), "GCUnregisterEvent");
            map->invalidation = nullptr;
            map->invalidation_source = nullptr;
            map->invalidation_ready = false;
        };
        if (enabled) {
            if (map->event_type != EVENT_REMOTE_DEVICE)
                return fail(IRIS_NOT_SUPPORTED, "enable invalidation", "feature invalidation requires a remote map");
            if (map->invalidation) {
                require(map->invalidation_ready, IRIS_INVALID_STATE, "enable invalidation",
                        "unsubscribe before retrying");
                return IRIS_OK;
            }
            if (!p.EventGetDataInfo)
                return fail(IRIS_NOT_SUPPORTED, "enable invalidation", "producer lacks EventGetDataInfo");
            const auto status = api("GCRegisterEvent", [&] {
                const auto register_on = [&](EVENTSRC_HANDLE source) {
                    map->invalidation_source = source;
                    return p.GCRegisterEvent(source, EVENT_FEATURE_INVALIDATE, &map->invalidation);
                };
                auto result = register_on(map->event_source);
                if (!map->invalidation && map->port.handle != map->event_source &&
                    (result == GC_ERR_INVALID_HANDLE || result == GC_ERR_NOT_IMPLEMENTED ||
                     result == GC_ERR_NOT_AVAILABLE)) {
                    // Some producers expose this notification only on the remote port.
                    const auto fallback = register_on(map->port.handle);
                    if (fallback != GC_ERR_INVALID_HANDLE || map->invalidation) result = fallback;
                }
                p.check(result, "GCRegisterEvent");
                require(map->invalidation, IRIS_PRODUCER_ERROR, "GCRegisterEvent", "missing event handle");
                return IRIS_OK;
            });
            if (status < 0 && map->invalidation) {
                const auto diagnostic = last;
                api("GCUnregisterEvent", [&] { unregister(); return IRIS_OK; });
                last = diagnostic;
            }
            if (!map->invalidation) map->invalidation_source = nullptr;
            map->invalidation_ready = status == IRIS_OK;
            return status;
        } else if (map->invalidation) {
            unregister();
        }
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_poll_invalidation(iris_node_map_t *map, size_t limit, size_t *processed)
{
    if (!processed) return invalid("poll invalidation");
    *processed = 0;
    if (!map || !limit) return invalid("poll invalidation");
    return api("poll invalidation", [&]() -> iris_status_t {
        std::lock_guard lock(*map->mutex);
        require(map->invalidation_ready, IRIS_INVALID_STATE, "poll invalidation", "invalidation is not enabled");
        auto &p = map->port.producer;
        std::vector<uint8_t> payload(256);
        while (*processed < limit) {
            size_t size;
            const auto status = receive_event_envelope(p, map->invalidation, payload, size, processed);
            if (status == IRIS_NO_EVENT) return IRIS_OK;
            if (status == IRIS_OUT_OF_RANGE) map->nodes._InvalidateNodes(); // The discarded name is unknown.
            if (status != IRIS_OK) return status;
            const auto name = text_query(p, "EventGetDataInfo", [&](char *out, size_t *length) {
                INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
                const auto result = p.EventGetDataInfo(map->invalidation, payload.data(), size,
                                                        EVENT_DATA_ID, &type, out, length);
                if (result == GC_ERR_SUCCESS && out)
                    require(type == INFO_DATATYPE_STRING, IRIS_PRODUCER_ERROR,
                            "EventGetDataInfo", "feature name is not a string");
                return result;
            });
            // An event can name a feature above cached terminal nodes, including
            // selector-indexed registers. Clear the map without changing selectors.
            if (map->nodes._GetNode(name.c_str())) map->nodes._InvalidateNodes();
        }
        return IRIS_OK;
    });
}
