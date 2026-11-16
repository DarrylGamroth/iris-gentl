#include "producer.hpp"

#include "cache.hpp"
#include "error.hpp"
#include "node_map.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <dlfcn.h>

namespace iris {

using namespace GenTL;

namespace {

namespace fs = std::filesystem;

struct ProducerEntry {
    std::weak_ptr<Producer> active;
    std::shared_ptr<Producer> closing;
};
std::map<std::string, ProducerEntry> producers;

std::string resolve(const char *name)
{
    require(name && *name, IRIS_INVALID_ARGUMENT, "producer", "missing CTI path");
    fs::path path(name);
    if (path.is_absolute()) {
        std::error_code error;
        auto canonical = fs::canonical(path, error);
        require(!error, IRIS_NOT_FOUND, "producer", "CTI path not found");
        return canonical.string();
    }
    require(!path.has_parent_path(), IRIS_INVALID_ARGUMENT, "producer", "expected absolute path or filename");
    const char *environment = std::getenv("GENICAM_GENTL64_PATH");
    std::string paths = environment ? environment : "", match;
    for (size_t begin = 0; begin < paths.size();) {
        const auto end = paths.find(':', begin);
        if (end != begin) {
            std::error_code error;
            auto candidate = fs::canonical(fs::path(paths.substr(begin, end - begin)) / path, error);
            if (!error) {
                require(match.empty() || match == candidate, IRIS_AMBIGUOUS, "producer", "multiple CTIs match filename");
                match = candidate.string();
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    require(!match.empty(), IRIS_NOT_FOUND, "producer", "CTI filename not found");
    return match;
}

} // namespace

alignas(cache_line_size) std::mutex registry_mutex;

Producer::~Producer()
{
    // Exception unwinding may reach here before ownership can be published.
    // On failure close() leaves the CTI mapped.
    if (library) close();
}

iris_status_t Producer::close()
{
    return api("producer close", [&]() -> iris_status_t {
        while (!interfaces.empty()) {
            const auto it = interfaces.begin();
            require(it->second.users == 0, IRIS_BUSY, "producer close", "interface still in use");
            check(release_interface(*this, it->first), "IFClose");
        }
        if (system) {
            const auto status = disable_node_map_events(map.get());
            if (status != IRIS_OK) return status;
            check(TLClose(system), "TLClose");
            system = nullptr;
            map.reset();
        }
        if (initialized) {
            check(GCCloseLib(), "GCCloseLib");
            initialized = false;
        }
        if (library) {
            if (dlclose(library) != 0) return fail(IRIS_IO_ERROR, "dlclose", dlerror());
            library = nullptr;
        }
        return IRIS_OK;
    });
}

iris_status_t close_if_unused(const std::shared_ptr<Producer> &producer)
{
    auto &entry = producers.at(producer->path);
    if (producer.use_count() > 1 + (entry.closing ? 1 : 0)) return IRIS_OK;
    const auto status = producer->close();
    if (status == IRIS_OK) entry.closing.reset();
    else entry.closing = producer;
    return status;
}

iris_status_t Producer::error(GC_ERROR code, const char *operation)
{
    if (code == GC_ERR_SUCCESS) return IRIS_OK;
    iris_status_t status = IRIS_PRODUCER_ERROR;
    switch (code) {
    case GC_ERR_NOT_IMPLEMENTED: case GC_ERR_NOT_AVAILABLE: status = IRIS_NOT_SUPPORTED; break;
    case GC_ERR_ACCESS_DENIED: status = IRIS_ACCESS_DENIED; break;
    case GC_ERR_INVALID_ID: case GC_ERR_INVALID_INDEX: status = IRIS_NOT_FOUND; break;
    case GC_ERR_BUSY: case GC_ERR_RESOURCE_IN_USE: status = IRIS_BUSY; break;
    case GC_ERR_OUT_OF_MEMORY: case GC_ERR_RESOURCE_EXHAUSTED: status = IRIS_NO_MEMORY; break;
    case GC_ERR_AMBIGUOUS: status = IRIS_AMBIGUOUS; break;
    case GC_ERR_BUFFER_TOO_SMALL: status = IRIS_BUFFER_TOO_SMALL; break;
    case GC_ERR_INVALID_PARAMETER: status = IRIS_INVALID_ARGUMENT; break;
    default: break;
    }
    fail(status, operation, "GenTL operation failed");
    last.native = code;
    last.valid = 1;
    GC_ERROR reported = 0;
    char message[sizeof(last.message)]{};
    size_t size = sizeof(message);
    if (GCGetLastError(&reported, message, &size) == GC_ERR_SUCCESS && reported == code && message[0]) {
        message[sizeof(message)-1] = 0;
        std::memcpy(last.message, message, sizeof(message));
    }
    return status;
}

void Producer::check(GC_ERROR code, const char *operation)
{
    if (code != GC_ERR_SUCCESS) throw Failure{error(code, operation)};
}

std::shared_ptr<Producer> load(const char *name)
{
    auto path = resolve(name);
    auto &entry = producers[path];
    if (entry.closing) {
        // A device still owns this partially closed producer. Its close
        // must finish before the CTI can be reopened.
        require(entry.closing.use_count() == 1, IRIS_BUSY, "producer load", "producer close pending");
        const auto status = entry.closing->close();
        if (status != IRIS_OK) throw Failure{status};
        entry.closing.reset();
    }
    if (auto existing = entry.active.lock()) return existing;
    auto p = std::make_shared<Producer>();
    p->path = path;
    p->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    require(p->library, IRIS_IO_ERROR, "dlopen", p->library ? "" : dlerror());
    // The loader can return the same object for different canonical paths.
    // Keep one GenTL lifetime for that object and drop the extra loader reference.
    for (auto &[key, known] : producers) {
        if (key == path) continue;
        if (known.closing && known.closing->library == p->library) {
            require(known.closing.use_count() == 1, IRIS_BUSY, "producer load", "producer close pending");
            const auto status = known.closing->close();
            if (status != IRIS_OK) throw Failure{status};
            known.closing.reset();
            break;
        }
        if (auto existing = known.active.lock(); existing && existing->library == p->library)
            return existing;
    }
#define LOAD(name) \
    p->name = reinterpret_cast<P##name>(dlsym(p->library, #name)); \
    require(p->name, IRIS_NOT_SUPPORTED, #name, "missing GenTL export");
    GENTL_FUNCTIONS(LOAD)
#undef LOAD
    p->EventGetDataInfo = reinterpret_cast<PEventGetDataInfo>(dlsym(p->library, "EventGetDataInfo"));
    p->EventGetInfo = reinterpret_cast<PEventGetInfo>(dlsym(p->library, "EventGetInfo"));
    p->GCWritePortStacked = reinterpret_cast<PGCWritePortStacked>(dlsym(p->library, "GCWritePortStacked"));
    p->GCGetNumPortURLs = reinterpret_cast<PGCGetNumPortURLs>(dlsym(p->library, "GCGetNumPortURLs"));
    p->GCGetPortURLInfo = reinterpret_cast<PGCGetPortURLInfo>(dlsym(p->library, "GCGetPortURLInfo"));
    p->DSGetNumBufferParts = reinterpret_cast<PDSGetNumBufferParts>(dlsym(p->library, "DSGetNumBufferParts"));
    p->DSGetBufferPartInfo = reinterpret_cast<PDSGetBufferPartInfo>(dlsym(p->library, "DSGetBufferPartInfo"));
    p->DSGetBufferChunkData = reinterpret_cast<PDSGetBufferChunkData>(dlsym(p->library, "DSGetBufferChunkData"));
    p->DSGetBufferInfoStacked = reinterpret_cast<PDSGetBufferInfoStacked>(dlsym(p->library, "DSGetBufferInfoStacked"));
    try {
        p->check(p->GCInitLib(), "GCInitLib");
        p->initialized = true;
        p->check(p->TLOpen(&p->system), "TLOpen");
    } catch (...) {
        const auto diagnostic = last;
        if (p->close() != IRIS_OK) entry.closing = p;
        last = diagnostic;
        throw;
    }
    entry.active = p;
    return p;
}

IF_HANDLE acquire_interface(Producer &p, const std::string &id)
{
    auto [it, inserted] = p.interfaces.try_emplace(id);
    if (inserted) {
        try {
            p.check(p.TLOpenInterface(p.system, id.c_str(), &it->second.handle), "TLOpenInterface");
        } catch (...) {
            if (!it->second.handle) p.interfaces.erase(it);
            throw;
        }
    }
    auto &iface = it->second;
    if (iface.failed_device) {
        p.check(p.DevClose(iface.failed_device), "DevClose");
        iface.failed_device = nullptr;
    }
    ++iface.users;
    return it->second.handle;
}

GC_ERROR release_interface(Producer &p, const std::string &id)
{
    auto it = p.interfaces.find(id);
    auto &iface = it->second;
    if (iface.users > 1) {
        --iface.users;
        return GC_ERR_SUCCESS;
    }
    if (iface.failed_device) {
        p.check(p.DevClose(iface.failed_device), "DevClose");
        iface.failed_device = nullptr;
    }
    const auto status = disable_node_map_events(iface.map.get());
    if (status != IRIS_OK) throw Failure{status};
    const auto rc = p.IFClose(iface.handle);
    if (rc == GC_ERR_SUCCESS) p.interfaces.erase(it);
    return rc;
}

void abandon_interface(Producer &p, const std::string &id) noexcept
{
    const auto diagnostic = last;
    const auto status = api("interface rollback", [&] {
        p.check(release_interface(p, id), "IFClose");
        return IRIS_OK;
    });
    if (status != IRIS_OK) {
        // The caller will never retry this reference. Keep the open handle
        // in the table so a later acquisition or producer close can recover.
        auto &iface = p.interfaces.find(id)->second;
        --iface.users;
    }
    last = diagnostic;
}

} // namespace iris
