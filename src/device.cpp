#include "device.hpp"

#include "error.hpp"
#include "gentl_info.hpp"
#include "node_map.hpp"
#include "producer.hpp"

#include <algorithm>
#include <iterator>
#include <vector>

using namespace GenTL;
using namespace iris;

namespace {

struct DeviceInfo {
    std::string path, interface, id, serial, vendor, model, display, transport;
};

std::vector<DeviceInfo> discover(Producer &p, uint64_t timeout, const char *filter = nullptr)
{
    bool8_t changed = 0;
    uint32_t count = 0;
    p.check(p.TLUpdateInterfaceList(p.system, &changed, timeout), "TLUpdateInterfaceList");
    p.check(p.TLGetNumInterfaces(p.system, &count), "TLGetNumInterfaces");
    std::vector<DeviceInfo> devices;
    for (uint32_t i = 0; i < count; ++i) {
        const auto id = text_query(p, "TLGetInterfaceID", [&](char *s, size_t *n) {
            return p.TLGetInterfaceID(p.system, i, s, n);
        });
        if (filter && id != filter) continue;
        auto iface = acquire_interface(p, id);
        try {
            uint32_t n = 0;
            p.check(p.IFUpdateDeviceList(iface, &changed, timeout), "IFUpdateDeviceList");
            p.check(p.IFGetNumDevices(iface, &n), "IFGetNumDevices");
            for (uint32_t j = 0; j < n; ++j) {
                auto device = text_query(p, "IFGetDeviceID", [&](char *s, size_t *size) {
                    return p.IFGetDeviceID(iface, j, s, size);
                });
                auto info = [&](DEVICE_INFO_CMD cmd) {
                    return text_query(p, "IFGetDeviceInfo", [&](char *s, size_t *size) {
                        INFO_DATATYPE type = 0;
                        return p.IFGetDeviceInfo(iface, device.c_str(), cmd, &type, s, size);
                    }, true);
                };
                devices.push_back({p.path, id, device, info(DEVICE_INFO_SERIAL_NUMBER),
                    info(DEVICE_INFO_VENDOR), info(DEVICE_INFO_MODEL),
                    info(DEVICE_INFO_DISPLAYNAME), info(DEVICE_INFO_TLTYPE)});
            }
        } catch (...) {
            abandon_interface(p, id);
            throw;
        }
        const auto closed = api("interface close", [&] {
            p.check(release_interface(p, id), "IFClose");
            return IRIS_OK;
        });
        if (closed != IRIS_OK) {
            // A failed discovery leaves no caller to release this reference.
            --p.interfaces.at(id).users;
            throw Failure{closed};
        }
    }
    return devices;
}

const char *optional(const std::string &s) { return s.empty() ? nullptr : s.c_str(); }

} // namespace

struct iris_context { size_t devices = 0; };
struct iris_device_list { std::vector<DeviceInfo> entries; };

iris_status_t iris_context_create(iris_context_t **out)
{
    if (!out) return invalid("context create");
    *out = nullptr;
    return api("context create", [&] { *out = new iris_context; return IRIS_OK; });
}
iris_status_t iris_context_close(iris_context_t **out)
{
    return api("context close", [&]() -> iris_status_t {
        if (!out || !*out) return invalid("context close");
        std::lock_guard lock(registry_mutex);
        if ((*out)->devices) return fail(IRIS_BUSY, "context close", "device still open");
        delete *out;
        *out = nullptr;
        return IRIS_OK;
    });
}
iris_status_t iris_discover(iris_context_t *context, const char *name, uint64_t timeout, iris_device_list_t **out)
{
    if (!out) return invalid("discover");
    *out = nullptr;
    if (!context) return invalid("discover");
    return api("discover", [&] {
        std::lock_guard lock(registry_mutex);
        auto producer = load(name);
        std::unique_ptr<iris_device_list> list;
        try {
            list = std::make_unique<iris_device_list>();
            list->entries = discover(*producer, timeout);
        } catch (...) {
            const auto error = last;
            close_if_unused(producer);
            last = error;
            throw;
        }
        const auto closed = close_if_unused(producer);
        if (closed != IRIS_OK) throw Failure{closed};
        *out = list.release();
        return IRIS_OK;
    });
}
size_t iris_device_list_count(const iris_device_list_t *list) { return list ? list->entries.size() : 0; }
iris_status_t iris_device_list_get(const iris_device_list_t *list, size_t index, iris_device_info_t *out)
{
    if (!list || !out || index >= list->entries.size()) return invalid("device list get");
    const auto &d = list->entries[index];
    *out = {d.path.c_str(), d.interface.c_str(), d.id.c_str(), optional(d.serial),
            optional(d.vendor), optional(d.model), optional(d.display), optional(d.transport)};
    return IRIS_OK;
}
void iris_device_list_close(iris_device_list_t **list)
{
    if (list) { delete *list; *list = nullptr; }
}
iris_status_t iris_device_open(iris_context_t *context, const iris_device_selector_t *selector,
                               iris_device_access_t access, uint64_t timeout, iris_device_t **out)
{
    const iris_device_open_options_t options{timeout, nullptr, access};
    return iris_device_open_with_options(context, selector, &options, out);
}

iris_status_t iris_device_open_with_options(iris_context_t *context, const iris_device_selector_t *selector,
                                            const iris_device_open_options_t *options, iris_device_t **out)
{
    if (!out) return invalid("device open");
    *out = nullptr;
    if (!context || !selector || !options || (selector->serial_number
            ? selector->device_id || !*selector->serial_number
            : !selector->interface_id || !selector->device_id) ||
        options->access < IRIS_DEVICE_READ_ONLY || options->access > IRIS_DEVICE_EXCLUSIVE ||
        (options->remote_xml_path && !*options->remote_xml_path))
        return invalid("device open");
    const auto access = options->access;
    const auto timeout = options->update_timeout_ms;
    return api("device open", [&] {
        std::lock_guard lock(registry_mutex);
        auto p = load(selector->producer);
        std::unique_ptr<iris_device> d;
        try {
            auto devices = discover(*p, timeout, selector->interface_id);
            auto matches = [&](const DeviceInfo &d) {
                return selector->serial_number ? d.serial == selector->serial_number : d.id == selector->device_id;
            };
            auto it = std::find_if(devices.begin(), devices.end(), matches);
            require(it != devices.end(), IRIS_NOT_FOUND, "device open", "device not found");
            require(std::find_if(std::next(it), devices.end(), matches) == devices.end(),
                    IRIS_AMBIGUOUS, "device open", "device selector is ambiguous");
            d = std::make_unique<iris_device>();
            d->context = context;
            d->producer = p;
            d->interface = it->interface;
            d->iface = acquire_interface(*p, d->interface);
            d->writable = access != IRIS_DEVICE_READ_ONLY;
            const auto flags = access == IRIS_DEVICE_CONTROL ? DEVICE_ACCESS_CONTROL :
                access == IRIS_DEVICE_EXCLUSIVE ? DEVICE_ACCESS_EXCLUSIVE : DEVICE_ACCESS_READONLY;
            bool8_t changed = 0;
            p->check(p->IFUpdateDeviceList(d->iface, &changed, timeout), "IFUpdateDeviceList");
            p->check(p->IFOpenDevice(d->iface, it->id.c_str(), flags, &d->handle), "IFOpenDevice");
            if (options->remote_xml_path) {
                const auto status = api("GenApi XML override", [&] {
                    d->maps[1] = load_remote_node_map(*d, options->remote_xml_path);
                    return IRIS_OK;
                });
                if (status != IRIS_OK) {
                    const auto diagnostic = last;
                    if (api("DevClose", [&] { return p->error(p->DevClose(d->handle), "DevClose"); }) == IRIS_OK)
                        d->handle = nullptr;
                    last = diagnostic;
                    throw Failure{status};
                }
            }
        } catch (...) {
            const auto diagnostic = last;
            if (d && d->iface) {
                if (d->handle) p->interfaces.at(d->interface).failed_device = d->handle;
                abandon_interface(*p, d->interface);
            }
            d.reset();
            close_if_unused(p);
            last = diagnostic;
            throw;
        }
        ++context->devices;
        *out = d.release();
        return IRIS_OK;
    });
}
iris_status_t iris_device_close(iris_device_t **out)
{
    if (!out || !*out) return invalid("device close");
    return api("device close", [&]() -> iris_status_t {
        std::lock_guard lock(registry_mutex);
        auto &d = **out;
        require(!d.streams, IRIS_BUSY, "device close", "stream still open");
        d.closing = true;
        if (d.handle) {
            const auto recovered = iris::close_failed_stream(d);
            if (recovered != IRIS_OK) return recovered;
            for (auto &map : d.maps) {
                const auto status = disable_node_map_events(map.get());
                if (status != IRIS_OK) return status;
            }
            d.producer->check(d.producer->DevClose(d.handle), "DevClose");
            d.handle = nullptr;
            for (auto &map : d.maps) map.reset();
        }
        if (d.iface) {
            d.producer->check(release_interface(*d.producer, d.interface), "IFClose");
            d.iface = nullptr;
        }
        const auto closed = close_if_unused(d.producer);
        if (closed != IRIS_OK) return closed;
        --d.context->devices;
        delete *out;
        *out = nullptr;
        return IRIS_OK;
    });
}
iris_status_t iris_device_stream_count(iris_device_t *d, size_t *out)
{
    return api("stream count", [&]() -> iris_status_t {
        if (!d || !out) return invalid("stream count");
        require(!d->closing, IRIS_INVALID_STATE, "stream count", "device close pending");
        uint32_t count = 0;
        const auto status = d->producer->error(d->producer->DevGetNumDataStreams(d->handle, &count), "DevGetNumDataStreams");
        if (status == IRIS_OK) *out = count;
        return status;
    });
}
