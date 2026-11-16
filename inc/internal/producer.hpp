#pragma once

#include "iris.h"

#include <GenTL.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace iris {

#define GENTL_FUNCTIONS(X) \
    X(GCInitLib) X(GCCloseLib) X(GCGetLastError) X(TLOpen) X(TLClose) \
    X(TLUpdateInterfaceList) X(TLGetNumInterfaces) X(TLGetInterfaceID) X(TLOpenInterface) \
    X(IFClose) X(IFUpdateDeviceList) X(IFGetNumDevices) X(IFGetDeviceID) X(IFGetDeviceInfo) X(IFOpenDevice) \
    X(DevClose) X(DevGetPort) X(DevGetNumDataStreams) X(DevGetDataStreamID) X(DevOpenDataStream) \
    X(DSClose) X(DSGetInfo) X(DSAllocAndAnnounceBuffer) X(DSAnnounceBuffer) X(DSRevokeBuffer) \
    X(DSQueueBuffer) X(DSFlushQueue) X(DSStartAcquisition) X(DSStopAcquisition) X(DSGetBufferInfo) \
    X(GCRegisterEvent) X(GCUnregisterEvent) X(EventGetData) X(EventKill) \
    X(GCGetPortURL) X(GCGetPortInfo) X(GCReadPort) X(GCWritePort)

struct Interface {
    GenTL::IF_HANDLE handle = nullptr;
    size_t users = 0;
    GenTL::DEV_HANDLE failed_device = nullptr;
    std::unique_ptr<iris_node_map> map;
};

struct Producer {
    void *library = nullptr;
    GenTL::TL_HANDLE system = nullptr;
    bool initialized = false;
    std::string path;
    std::map<std::string, Interface> interfaces;
    std::unique_ptr<iris_node_map> map;
#define DECLARE(name) GenTL::P##name name = nullptr;
    GENTL_FUNCTIONS(DECLARE)
#undef DECLARE
    GenTL::PEventGetDataInfo EventGetDataInfo = nullptr;
    GenTL::PEventGetInfo EventGetInfo = nullptr;
    GenTL::PGCWritePortStacked GCWritePortStacked = nullptr;
    GenTL::PGCGetNumPortURLs GCGetNumPortURLs = nullptr;
    GenTL::PGCGetPortURLInfo GCGetPortURLInfo = nullptr;
    GenTL::PDSGetNumBufferParts DSGetNumBufferParts = nullptr;
    GenTL::PDSGetBufferPartInfo DSGetBufferPartInfo = nullptr;
    GenTL::PDSGetBufferChunkData DSGetBufferChunkData = nullptr;
    GenTL::PDSGetBufferInfoStacked DSGetBufferInfoStacked = nullptr;

    ~Producer();
    iris_status_t close();
    iris_status_t error(GenTL::GC_ERROR code, const char *operation);
    void check(GenTL::GC_ERROR code, const char *operation);
};

// Hold the registry lock while loading producers or changing interface ownership.
extern std::mutex registry_mutex;
std::shared_ptr<Producer> load(const char *name);
iris_status_t close_if_unused(const std::shared_ptr<Producer> &producer);
GenTL::IF_HANDLE acquire_interface(Producer &p, const std::string &id);
GenTL::GC_ERROR release_interface(Producer &p, const std::string &id);
void abandon_interface(Producer &p, const std::string &id) noexcept;

} // namespace iris
