#include <GenTL.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <stdexcept>

using namespace GenTL;

namespace {
char scenario[32];
GC_ERROR last_error;
std::array<unsigned char, 16> pixels{};
unsigned progressive_poll = 0;
std::chrono::steady_clock::time_point next_progressive_frame;
void *announced_memory;
bool queued, delivered;
unsigned completions;
unsigned failures, interface_closes;
bool armed, system_open, interface_open, failed_device_open, partial_stream;
bool invalidation_registered, invalidation_pending;
EVENTSRC_HANDLE invalidation_source = nullptr;
struct EventQueue {
    unsigned next = 0;
    bool active = false;
};
std::array<EventQueue, 6> event_queues;

// The two TriggerMode registers occupy independent selector banks.
constexpr uint64_t xml_address = 0x1000;
constexpr char node_xml[] = R"xml(
<RegisterDescription xmlns="http://www.genicam.org/GenApi/Version_1_1"
    ModelName="NodeFixture" VendorName="IrisTests" ToolTip="Writable node fixture"
    StandardNameSpace="None" SchemaMajorVersion="1" SchemaMinorVersion="1"
    SchemaSubMinorVersion="0" MajorVersion="1" MinorVersion="0" SubMinorVersion="0"
    ProductGuid="cc24e823-70e8-41bf-bab2-3650c81911db"
    VersionGuid="664cdbf7-f87a-4937-a720-4b020fc22f94">
  <Category Name="Root">
    <pFeature>AcquisitionControl</pFeature><pFeature>ChunkCounter</pFeature>
    <pFeature>ReverseX</pFeature><pFeature>DeviceUserID</pFeature>
    <pFeature>Width</pFeature><pFeature>ExposureTime</pFeature><pFeature>TriggerSoftware</pFeature>
    <pFeature>RawData</pFeature><pFeature>EventCounter</pFeature><pFeature>EventWideCounter</pFeature>
    <pFeature>WidthUnavailable</pFeature>
  </Category>
  <Category Name="AcquisitionControl" NameSpace="Standard">
    <pFeature>TriggerSelector</pFeature><pFeature>TriggerMode</pFeature>
  </Category>
  <Enumeration Name="TriggerSelector" NameSpace="Standard">
    <ToolTip>Selects a trigger.</ToolTip><Description>Selects the trigger to configure.</Description>
    <DisplayName>Trigger Selector</DisplayName><Visibility>Expert</Visibility>
    <EnumEntry Name="FrameStart"><Value>0</Value></EnumEntry>
    <EnumEntry Name="ExposureStart"><Value>1</Value></EnumEntry>
    <pValue>TriggerSelectorReg</pValue><pSelected>TriggerMode</pSelected>
  </Enumeration>
  <Enumeration Name="TriggerMode" NameSpace="Standard">
    <EnumEntry Name="Off"><Value>0</Value></EnumEntry>
    <EnumEntry Name="On"><Value>1</Value></EnumEntry>
    <pValue>TriggerModeReg</pValue>
  </Enumeration>
  <Boolean Name="ReverseX" NameSpace="Standard">
    <pValue>ReverseXReg</pValue><OnValue>1</OnValue><OffValue>0</OffValue>
  </Boolean>
  <IntReg Name="TriggerSelectorReg">
    <Address>0</Address><Length>4</Length><AccessMode>RW</AccessMode>
    <pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="TriggerModeReg">
    <Address>4</Address><pIndex Offset="4">TriggerSelectorReg</pIndex>
    <Length>4</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
    <Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="ReverseXReg">
    <Address>12</Address><Length>4</Length><AccessMode>RW</AccessMode>
    <pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <StringReg Name="DeviceUserID" NameSpace="Standard">
    <Address>16</Address><Length>16</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
  </StringReg>
  <Integer Name="Width" NameSpace="Standard">
    <pAlias>WidthRaw</pAlias>
    <Value>16</Value><Min>16</Min><pMax>WidthMaxReg</pMax><Inc>16</Inc><Unit>px</Unit>
  </Integer>
  <Integer Name="WidthRaw">
    <Value>16</Value><Min>16</Min><Max>64</Max><Inc>16</Inc>
    <Representation>HexNumber</Representation>
    <ValidValueSet>16;32;64</ValidValueSet>
  </Integer>
  <Integer Name="WidthUnavailable">
    <pIsAvailable>Unavailable</pIsAvailable><Value>0</Value>
  </Integer>
  <Integer Name="Unavailable"><Value>0</Value></Integer>
  <Float Name="ExposureTime" NameSpace="Standard">
    <Value>100</Value><Min>100</Min><Max>1000</Max><Inc>50</Inc><Unit>us</Unit>
    <Representation>Logarithmic</Representation><DisplayNotation>Fixed</DisplayNotation>
    <DisplayPrecision>2</DisplayPrecision>
  </Float>
  <Converter Name="WidthScaled">
    <FormulaTo>FROM * 16</FormulaTo><FormulaFrom>TO / 16</FormulaFrom>
    <pValue>WidthRaw</pValue><Slope>Increasing</Slope>
  </Converter>
  <Command Name="TriggerSoftware" NameSpace="Standard">
    <pValue>TriggerSoftwareReg</pValue><CommandValue>1</CommandValue>
  </Command>
  <IntReg Name="TriggerSoftwareReg">
    <Address>32</Address><Length>4</Length><AccessMode>RW</AccessMode>
    <pPort>Device</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Integer Name="TriggerCount">
    <pValue>TriggerCountReg</pValue>
  </Integer>
  <IntReg Name="TriggerCountReg">
    <Address>36</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>Device</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="WidthMaxReg">
    <Address>40</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Command Name="ChangeWidthMax">
    <pValue>ChangeWidthMaxReg</pValue><CommandValue>1</CommandValue>
  </Command>
  <IntReg Name="ChangeWidthMaxReg">
    <Address>44</Address><Length>4</Length><AccessMode>WO</AccessMode>
    <pPort>Device</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Register Name="RawData">
    <Address>48</Address><Length>4</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
  </Register>
  <Integer Name="Height">
    <Value>16</Value><Min>16</Min><pMax>HeightMaxReg</pMax><Inc>16</Inc>
  </Integer>
  <IntReg Name="HeightMaxReg">
    <Address>52</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>Device</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Command Name="ChangeHeightMax">
    <pValue>ChangeHeightMaxReg</pValue><CommandValue>1</CommandValue>
  </Command>
  <IntReg Name="ChangeHeightMaxReg">
    <Address>56</Address><Length>4</Length><AccessMode>WO</AccessMode>
    <pPort>Device</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="ChunkCounter">
    <Address>0</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>ChunkPort</pPort><Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="ChunkPort"><ChunkID>1234</ChunkID></Port>
  <FloatReg Name="ChunkTemperature">
    <Address>0</Address><Length>8</Length><AccessMode>RO</AccessMode>
    <pPort>ChunkTypedPort</pPort><Endianess>LittleEndian</Endianess>
  </FloatReg>
  <StringReg Name="ChunkLabel">
    <Address>8</Address><Length>8</Length><AccessMode>RO</AccessMode><pPort>ChunkTypedPort</pPort>
  </StringReg>
  <IntReg Name="ChunkFlagsReg">
    <Address>16</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>ChunkTypedPort</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Boolean Name="ChunkEnabled">
    <pValue>ChunkFlagsReg</pValue><OnValue>1</OnValue><OffValue>0</OffValue>
  </Boolean>
  <Enumeration Name="ChunkChoice">
    <EnumEntry Name="First"><Value>0</Value></EnumEntry>
    <EnumEntry Name="Second"><Value>1</Value></EnumEntry>
    <pValue>ChunkFlagsReg</pValue>
  </Enumeration>
  <Register Name="ChunkRaw">
    <Address>20</Address><Length>4</Length><AccessMode>RO</AccessMode><pPort>ChunkTypedPort</pPort>
  </Register>
  <IntReg Name="ChunkWide">
    <Address>24</Address><Length>8</Length><AccessMode>RO</AccessMode>
    <pPort>ChunkTypedPort</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="ChunkTypedPort"><ChunkID>1235</ChunkID></Port>
  <IntReg Name="ChunkMissing">
    <Address>0</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>ChunkMissingPort</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="ChunkMissingPort"><ChunkID>1236</ChunkID></Port>
  <IntSwissKnife Name="ChunkMixed">
    <pVariable Name="A">ChunkCounter</pVariable><pVariable Name="B">ChunkMissing</pVariable>
    <Formula>A+B</Formula>
  </IntSwissKnife>
  <IntReg Name="EventCounter">
    <Address>0</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>EventPort</pPort><Cachable>WriteThrough</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="EventPort"><EventID>1234</EventID></Port>
  <IntReg Name="EventExposureEndTimestamp" NameSpace="Standard">
    <Address>4</Address><Length>8</Length><AccessMode>RO</AccessMode>
    <pPort>EventPort</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="EventWideCounter">
    <Address>0</Address><Length>4</Length><AccessMode>RO</AccessMode>
    <pPort>EventWidePort</pPort><Endianess>LittleEndian</Endianess>
  </IntReg>
  <Port Name="EventWidePort"><EventID>112233445566778899</EventID></Port>
  <IntSwissKnife Name="CombinedCounter">
    <pVariable Name="A">EventCounter</pVariable><pVariable Name="B">EventWideCounter</pVariable>
    <Formula>A+B</Formula>
  </IntSwissKnife>
  <Enumeration Name="AlternateMode">
    <EnumEntry Name="Off"><Value>0</Value></EnumEntry>
    <pValue>Unavailable</pValue>
  </Enumeration>
  <Port Name="Device"/>
</RegisterDescription>)xml";
std::array<unsigned char, 100> registers{};
size_t stacked_calls = 0;
size_t scalar_writes = 0;
void (*write_observer)(void *, uint64_t) = nullptr;
void *write_observer_context = nullptr;

bool mode(const char *name) { return std::strcmp(scenario, name) == 0; }
bool progressive() { return std::strncmp(scenario, "progressive", 11) == 0; }

bool module_events()
{
    return mode("events") || mode("events-rollback") || mode("events-oversized") || mode("events-snapshot");
}
bool chunk_data_size() { return mode("chunk-data-size") || mode("chunk-bounds-data-size"); }
size_t event_slot(EVENTSRC_HANDLE source, EVENT_TYPE type)
{
    const auto index = reinterpret_cast<uintptr_t>(source);
    if (type == EVENT_REMOTE_DEVICE) return index == 3 ? 4 : 0;
    return type == EVENT_MODULE && index && index <= 5 && index != 4 ? index : 0;
}
size_t event_handle_slot(EVENT_HANDLE event)
{
    const auto value = reinterpret_cast<uintptr_t>(event);
    return value > 20 && value < 26 ? value - 20 : 0;
}
GC_ERROR error(GC_ERROR code) { return last_error = code; }
GC_ERROR text(const char *value, char *out, size_t *size)
{
    const auto n = std::strlen(value) + 1;
    if (!out || *size < n) { *size = n; return error(GC_ERR_BUFFER_TOO_SMALL); }
    std::memcpy(out, value, n);
    *size = n;
    return GC_ERR_SUCCESS;
}
template<class T>
GC_ERROR info(INFO_DATATYPE kind, T value, INFO_DATATYPE *type, void *out, size_t *size)
{
    if (*size < sizeof(value)) return error(GC_ERR_BUFFER_TOO_SMALL);
    *type = kind;
    *size = sizeof(value);
    std::memcpy(out, &value, sizeof(value));
    return GC_ERR_SUCCESS;
}
void *handle(unsigned id) { return reinterpret_cast<void *>(static_cast<uintptr_t>(id)); }
bool node_port(PORT_HANDLE port)
{
    if (progressive()) return port == handle(4);
    if (module_events()) return port == handle(1) || port == handle(2) || port == handle(3) ||
                                port == handle(4) || port == handle(5);
    return (mode("nodes") || mode("invalidation-rollback") || mode("invalidation-malformed") ||
            mode("invalidation-oversized") || mode("port-access") ||
            std::strncmp(scenario, "xml-", 4) == 0 || std::strncmp(scenario, "stacking", 8) == 0 ||
            mode("example") || mode("chunk") || mode("chunk-typed") || mode("bad-chunk") || chunk_data_size() ||
            mode("gendc-chunk") || mode("gendc-xml") || mode("multipart-chunk") ||
            mode("multipart-chunk-overlap") || mode("multipart-chunk-only") || mode("chunk-duplicate") ||
            mode("chunk-overlap") || mode("gendc-chunk-duplicate")) &&
           (port == handle(1) || port == handle(2) || port == handle(4));
}
bool gendc_payload() { return std::strncmp(scenario, "gendc", 5) == 0; }
bool multipart_payload() { return std::strncmp(scenario, "multipart", 9) == 0; }
bool multipart_chunk() { return std::strncmp(scenario, "multipart-chunk", 15) == 0; }
bool gendc_chunk() { return mode("gendc-chunk") || mode("gendc-chunk-duplicate"); }
size_t gendc_bytes()
{
    if (mode("gendc-xml") || mode("gendc-alias")) return 280;
    return mode("gendc-chunk-duplicate") ? 208 : 200;
}

void fill_gendc(unsigned char *data)
{
    const auto put = [&](size_t offset, auto value) {
        std::memcpy(data + offset, &value, sizeof(value));
    };
    std::memset(data, 0, gendc_bytes());
    std::memcpy(data, "GNDC", 4);
    data[4] = mode("gendc-version") ? 2 : 1;
    put(8, uint16_t{0x1000}); put(12, uint32_t{64});
    if (mode("gendc-xml") || mode("gendc-alias")) {
        put(32, uint64_t{24}); put(40, uint64_t{256});
        put(48, uint32_t{256}); put(52, uint32_t{1}); put(56, uint64_t{64});
        put(64, uint16_t{0x2000}); put(68, uint32_t{64}); put(96, uint64_t{0x8001});
        put(104, uint32_t{0x01080116}); put(110, uint16_t{2});
        put(112, uint64_t{128}); put(120, uint64_t{mode("gendc-alias") ? 128u : 192u});
        put(128, uint16_t{0x4000}); put(132, uint32_t{64}); put(136, uint32_t{0x01080116});
        put(144, uint64_t{256}); put(152, uint64_t{16}); put(160, uint64_t{256});
        put(168, uint64_t{12}); put(176, uint16_t{4});
        put(192, uint16_t{0x4001}); put(194, uint16_t{1u << 9});
        put(196, uint32_t{64}); put(200, uint32_t{0x01080116});
        put(208, uint64_t{272}); put(216, uint64_t{8}); put(224, uint64_t{272}); put(232, uint64_t{8});
        put(256, uint32_t{42}); put(260, uint32_t{0x1234}); put(264, uint32_t{4});
        std::memcpy(data + 272, "<other/>", 8);
        return;
    }
    const uint64_t chunk_bytes = mode("gendc-chunk-duplicate") ? 24 : 12;
    const uint64_t data_bytes = gendc_bytes() - 184;
    put(32, data_bytes); put(40, uint64_t{184});
    put(48, uint32_t{184}); put(52, uint32_t{1}); put(56, uint64_t{64});
    put(64, uint16_t{0x2000}); put(68, uint32_t{56});
    put(76, uint16_t{7}); put(78, uint16_t{9}); put(96, uint64_t{gendc_chunk() ? 0x8001u : 1u});
    put(110, uint16_t{1}); put(112, uint64_t{120});
    put(120, static_cast<uint16_t>(gendc_chunk() ? 0x4000 : 0x4200)); put(124, uint32_t{64});
    put(128, uint32_t{gendc_chunk() ? 0x01080116u : 0x01080001u});
    put(104, uint32_t{gendc_chunk() ? 0x01080116u : 0x01080001u});
    put(144, uint64_t{mode("gendc-bounds") ? 32u : gendc_chunk() ? data_bytes : 8u});
    put(152, uint64_t{184});
    if (mode("gendc-gap")) put(152, uint64_t{192});
    put(136, uint64_t{mode("gendc-gap") ? 192u : 184u});
    if (mode("gendc-prefetch")) put(24, uint16_t{1});
    if (mode("gendc-h264-short")) put(120, uint16_t{0x4203});
    if (mode("gendc-custom")) {
        put(120, uint16_t{0x42f0});
        put(124, uint32_t{40});
        put(48, uint32_t{160});
    }
    if (gendc_chunk()) { put(160, chunk_bytes); put(168, static_cast<uint16_t>(data_bytes - chunk_bytes)); }
    else { put(160, uint32_t{4}); put(164, uint32_t{2}); }
    put(184, uint32_t{42});
    if (gendc_chunk()) {
        put(188, uint32_t{0x1234});
        put(192, uint32_t{4});
    }
    if (mode("gendc-chunk-duplicate")) {
        put(196, uint32_t{43});
        put(200, uint32_t{0x1234});
        put(204, uint32_t{4});
    }
}

}

#define EXPORT(name) extern "C" GC_IMPORT_EXPORT GC_ERROR GC_CALLTYPE name
extern "C" GC_IMPORT_EXPORT void iris_test_observe_writes(void (*observer)(void *, uint64_t), void *context)
{
    write_observer = observer;
    write_observer_context = context;
}
extern "C" GC_IMPORT_EXPORT void iris_test_stack_counts(size_t *stacked, size_t *scalar)
{
    *stacked = stacked_calls;
    *scalar = scalar_writes;
}

EXPORT(GCInitLib)()
{
    const char *value = std::getenv("IRIS_FAULT");
    std::strncpy(scenario, value ? value : "", sizeof(scenario) - 1);
    queued = delivered = false;
    completions = failures = interface_closes = 0;
    armed = system_open = interface_open = failed_device_open = partial_stream = false;
    registers.fill(0);
    registers[40] = registers[52] = 64;
    if (progressive()) {
        registers[64] = mode("progressive-no-frame-start") ? 1 : 0;
        registers[68] = registers[72] = 1; // Start with both trigger banks enabled.
        registers[76] = registers[80] = 1; // Both banks initially select Line1.
        registers[84] = 1;                // Continuous.
        registers[96] = !mode("progressive-no-frame-start");
    }
    stacked_calls = scalar_writes = 0;
    invalidation_registered = invalidation_pending = false;
    event_queues = {};
    return 0;
}
EXPORT(GCCloseLib)()
{
    if (system_open) std::abort();
    if (mode("close-library") && !failures++) return error(GC_ERR_BUSY);
    return 0;
}
EXPORT(GCGetLastError)(GC_ERROR *code, char *out, size_t *size)
{ *code = last_error; return text("injected producer result", out, size); }
EXPORT(TLOpen)(TL_HANDLE *out) { system_open = true; *out = handle(1); return 0; }
EXPORT(TLClose)(TL_HANDLE)
{
    if (!system_open || interface_open || event_queues[1].active) std::abort();
    if (mode("close-system") && !failures++) return error(GC_ERR_BUSY);
    system_open = false;
    return 0;
}
EXPORT(TLUpdateInterfaceList)(TL_HANDLE, bool8_t *, uint64_t) { return 0; }
EXPORT(TLGetNumInterfaces)(TL_HANDLE, uint32_t *out) { *out = 1; return 0; }
EXPORT(TLGetInterfaceID)(TL_HANDLE, uint32_t, char *out, size_t *size)
{ return text("test-interface", out, size); }
EXPORT(TLOpenInterface)(TL_HANDLE, const char *, IF_HANDLE *out)
{
    if (mode("throw-interface") && !failures++) throw std::runtime_error("interface open");
    if (mode("interface")) return error(GC_ERR_ACCESS_DENIED);
    interface_open = true;
    *out = handle(2); return 0;
}
EXPORT(IFClose)(IF_HANDLE)
{
    if (!interface_open || failed_device_open || event_queues[2].active) std::abort();
    ++interface_closes;
    if ((mode("close-interface") && interface_closes == 2) ||
        (mode("discover-close") && interface_closes == 1)) return error(GC_ERR_BUSY);
    interface_open = false;
    return 0;
}
EXPORT(IFUpdateDeviceList)(IF_HANDLE, bool8_t *, uint64_t)
{ return mode("discover") ? error(GC_ERR_IO) : GC_ERR_SUCCESS; }
EXPORT(IFGetNumDevices)(IF_HANDLE, uint32_t *out) { *out = mode("ambiguous") || mode("nodes") || mode("throw-device") ? 2 : 1; return 0; }
EXPORT(IFGetDeviceID)(IF_HANDLE, uint32_t index, char *out, size_t *size)
{ return text(index ? "test-device-2" : "test-device", out, size); }
EXPORT(IFGetDeviceInfo)(IF_HANDLE, const char *, DEVICE_INFO_CMD, INFO_DATATYPE *, void *out, size_t *size)
{ return text("test-serial", static_cast<char *>(out), size); }
EXPORT(IFOpenDevice)(IF_HANDLE, const char *id, DEVICE_ACCESS_FLAGS, DEV_HANDLE *out)
{
    if (mode("device")) return error(GC_ERR_BUSY);
    const bool second = std::strcmp(id, "test-device-2") == 0;
    if (mode("throw-device") && second) {
        *out = handle(9);
        failed_device_open = true;
        throw std::runtime_error("device open");
    }
    *out = handle(second ? 9 : 3);
    return 0;
}
EXPORT(DevClose)(DEV_HANDLE device)
{
    if (mode("invalidation-rollback") && invalidation_registered) std::abort();
    if (event_queues[3].active || event_queues[4].active) std::abort();
    if (partial_stream) return error(GC_ERR_BUSY);
    if (mode("throw-device") && device == handle(9)) {
        if (!failed_device_open) std::abort();
        failed_device_open = false;
    }
    return 0;
}
EXPORT(DevGetPort)(DEV_HANDLE, PORT_HANDLE *out) { *out = handle(4); return 0; }
EXPORT(DevGetNumDataStreams)(DEV_HANDLE, uint32_t *out) { *out = mode("siblings") ? 2 : 1; return 0; }
EXPORT(DevGetDataStreamID)(DEV_HANDLE, uint32_t index, char *out, size_t *size)
{ return text(index ? "test-stream-2" : "test-stream", out, size); }
EXPORT(DevOpenDataStream)(DEV_HANDLE, const char *id, DS_HANDLE *out)
{
    if (partial_stream) std::abort();
    *out = handle(std::strcmp(id, "test-stream-2") == 0 ? 10 : 5);
    if (mode("partial-stream-throw") || mode("partial-stream-error")) {
        partial_stream = true;
        if (mode("partial-stream-throw")) throw std::runtime_error("partial stream open");
        return error(GC_ERR_IO);
    }
    return GC_ERR_SUCCESS;
}
EXPORT(DSClose)(DS_HANDLE)
{
    if (event_queues[5].active) std::abort();
    if (partial_stream && !failures++) return error(GC_ERR_BUSY);
    partial_stream = false;
    return GC_ERR_SUCCESS;
}
EXPORT(DSGetInfo)(DS_HANDLE, STREAM_INFO_CMD cmd, INFO_DATATYPE *type, void *out, size_t *size)
{
    if (cmd == STREAM_INFO_IS_GRABBING) {
        if (mode("armed-error") || mode("armed-throw")) return error(GC_ERR_NOT_AVAILABLE);
        return info(INFO_DATATYPE_BOOL8, bool8_t(armed), type, out, size);
    }
    if (cmd == STREAM_INFO_DEFINES_PAYLOADSIZE) return info(INFO_DATATYPE_BOOL8, bool8_t{1}, type, out, size);
    return info(INFO_DATATYPE_SIZET, size_t{cmd == STREAM_INFO_BUF_ANNOUNCE_MIN ? 1u :
                       cmd == STREAM_INFO_PAYLOAD_SIZE && gendc_payload() ? gendc_bytes() :
                       cmd == STREAM_INFO_PAYLOAD_SIZE && mode("chunk-typed") ? 52u : 16u}, type, out, size);
}
EXPORT(DSAllocAndAnnounceBuffer)(DS_HANDLE, size_t, void *, BUFFER_HANDLE *out)
{
    if (mode("allocate")) return error(GC_ERR_OUT_OF_MEMORY);
    if (mode("announce")) return error(GC_ERR_NOT_IMPLEMENTED);
    *out = handle(6); return 0;
}
EXPORT(DSAnnounceBuffer)(DS_HANDLE, void *memory, size_t, void *, BUFFER_HANDLE *out)
{
    if (mode("caller-unsupported")) return error(GC_ERR_NOT_IMPLEMENTED);
    if (announced_memory) std::abort();
    announced_memory = memory;
    if (mode("chunk-typed")) {
        auto *data = static_cast<unsigned char *>(memory);
        std::memset(data, 0, 52);
        const double temperature = 12.5;
        const uint32_t flags = 1;
        const unsigned char raw[] = {0xde, 0xad, 0, 0xff};
        const int64_t wide = 9007199254740993;
        std::memcpy(data + 20, &temperature, sizeof temperature);
        std::memcpy(data + 28, "sim", 4);
        std::memcpy(data + 36, &flags, sizeof flags);
        std::memcpy(data + 40, raw, sizeof raw);
        std::memcpy(data + 44, &wide, sizeof wide);
    }
    if (gendc_payload()) fill_gendc(static_cast<unsigned char *>(memory));
    if (mode("chunk") || mode("bad-chunk") || chunk_data_size() || multipart_chunk()) {
        const uint32_t value = 42;
        std::memcpy(static_cast<unsigned char *>(memory) + (multipart_chunk() ? 40 : 8), &value, sizeof(value));
    }
    *out = handle(6);
    if (mode("caller-rollback")) return error(GC_ERR_IO);
    return 0;
}
EXPORT(DSRevokeBuffer)(DS_HANDLE, BUFFER_HANDLE, void **, void **)
{
    if (armed) std::abort(); // DMA may still own the buffer.
    if ((mode("revoke") || mode("caller-rollback")) && !failures++) return error(GC_ERR_BUSY);
    announced_memory = nullptr;
    return 0;
}
EXPORT(DSQueueBuffer)(DS_HANDLE, BUFFER_HANDLE)
{
    if (mode("throw-requeue") && delivered) throw std::runtime_error("injected requeue exception");
    if (mode("queue") || (mode("requeue") && delivered)) return error(GC_ERR_IO);
    queued = true;
    progressive_poll = 0;
    if (mode("progressive-paced"))
        next_progressive_frame = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    return 0;
}
EXPORT(DSFlushQueue)(DS_HANDLE, ACQ_QUEUE_TYPE)
{
    if (armed) std::abort();
    queued = false;
    return 0;
}
EXPORT(DSStartAcquisition)(DS_HANDLE, ACQ_START_FLAGS, uint64_t)
{
    if (mode("start")) return error(GC_ERR_IO);
    armed = true;
    if (mode("armed-throw")) throw std::runtime_error("start threw after arming");
    if (mode("armed-error")) return error(GC_ERR_IO);
    return GC_ERR_SUCCESS;
}
EXPORT(DSStopAcquisition)(DS_HANDLE, ACQ_STOP_FLAGS)
{
    if (mode("throw-stop") && !failures++) throw std::runtime_error("injected stop exception");
    if ((mode("stop") || mode("armed-error") || mode("armed-throw")) && !failures++)
        return error(GC_ERR_BUSY);
    armed = false;
    return 0;
}
EXPORT(DSGetBufferInfo)(DS_HANDLE, BUFFER_HANDLE, BUFFER_INFO_CMD cmd, INFO_DATATYPE *type, void *out, size_t *size)
{
    switch (cmd) {
    case BUFFER_INFO_BASE:
        return info(INFO_DATATYPE_PTR, announced_memory && !mode("caller-wrong-base") ?
                    announced_memory : static_cast<void *>(pixels.data()), type, out, size);
    case BUFFER_INFO_IS_ACQUIRING:
        if (!progressive()) return error(GC_ERR_NOT_IMPLEMENTED);
        return info(INFO_DATATYPE_BOOL8, bool8_t(queued && progressive_poll > 0), type, out, size);
    case BUFFER_INFO_IS_INCOMPLETE:
        if (mode("progressive-incomplete"))
            return info(INFO_DATATYPE_BOOL8, bool8_t{1}, type, out, size);
        if (mode("multipart-no-status")) return error(GC_ERR_NOT_IMPLEMENTED);
        return info(INFO_DATATYPE_BOOL8, bool8_t(mode("incomplete")), type, out, size);
    case BUFFER_INFO_IMAGEPRESENT: return info(INFO_DATATYPE_BOOL8, bool8_t{1}, type, out, size);
    case BUFFER_INFO_PAYLOADTYPE: return info(INFO_DATATYPE_SIZET, size_t(gendc_payload() ? PAYLOAD_TYPE_GENDC : multipart_payload() ? PAYLOAD_TYPE_MULTI_PART :
                    mode("chunk-typed") ? PAYLOAD_TYPE_CHUNK_ONLY :
                    mode("payload") ? PAYLOAD_TYPE_RAW_DATA : PAYLOAD_TYPE_IMAGE), type, out, size);
    case BUFFER_INFO_SIZE_FILLED:
        if (progressive() && queued) {
            const size_t bytes = progressive_poll == 1 ? 6 :
                mode("progressive-regress") ? 1 : 12;
            return info(INFO_DATATYPE_SIZET, bytes, type, out, size);
        }
        if (mode("multipart-no-size") || chunk_data_size()) return error(GC_ERR_NOT_IMPLEMENTED);
        return info(INFO_DATATYPE_SIZET, size_t(mode("gendc-gap") ? 192 : gendc_payload() ? gendc_bytes() :
                    mode("multipart-chunk-only") ? 4 : multipart_chunk() ? 20 : mode("chunk-typed") ? 52 :
                    mode("invalid") ? 17 : mode("short-image") ? 12 : 16), type, out, size);
    case BUFFER_INFO_SIZE:
        return info(INFO_DATATYPE_SIZET, size_t{gendc_payload() ? gendc_bytes() : multipart_chunk() ? 48u : mode("chunk-typed") ? 52u :
                    mode("multipart-gap") || mode("multipart-no-size") ? 32u : 16u}, type, out, size);
    case BUFFER_INFO_DATA_SIZE:
        if (chunk_data_size()) return info(INFO_DATATYPE_SIZET, size_t{mode("chunk-data-size") ? 12u : 8u}, type, out, size);
        if (mode("payload")) return info(INFO_DATATYPE_SIZET, size_t{16}, type, out, size);
        return error(GC_ERR_NOT_IMPLEMENTED);
    case BUFFER_INFO_CONTAINS_CHUNKDATA:
        if (mode("payload") || mode("chunk") || mode("chunk-typed") || mode("bad-chunk") || chunk_data_size() || multipart_chunk() ||
            mode("chunk-duplicate") || mode("chunk-overlap"))
            return info(INFO_DATATYPE_BOOL8, bool8_t{1}, type, out, size);
        return error(GC_ERR_NOT_IMPLEMENTED);
    case BUFFER_INFO_IS_COMPOSITE:
        return info(INFO_DATATYPE_BOOL8, bool8_t(mode("multipart-composite")), type, out, size);
    case BUFFER_INFO_CHUNKLAYOUTID:
        return info(INFO_DATATYPE_UINT64, uint64_t{123}, type, out, size);
    case BUFFER_INFO_WIDTH:
        return info(INFO_DATATYPE_SIZET, size_t{progressive() ? 2u : mode("packed-odd") ? 3u : 4u}, type, out, size);
    case BUFFER_INFO_HEIGHT:
        return info(INFO_DATATYPE_SIZET, size_t{mode("packed") || mode("packed-odd") ? 2u : 4u}, type, out, size);
    case BUFFER_INFO_DELIVERED_IMAGEHEIGHT:
        if (mode("height-short") || mode("height-invalid"))
            return info(INFO_DATATYPE_SIZET, size_t{mode("height-short") ? 3u : 5u}, type, out, size);
        return error(GC_ERR_NOT_IMPLEMENTED);
    case BUFFER_INFO_IMAGEOFFSET:
        return info(INFO_DATATYPE_SIZET, size_t{mode("packed") ? 2u : 0u}, type, out, size);
    case BUFFER_INFO_XPADDING:
        return info(INFO_DATATYPE_SIZET, size_t{mode("packed") ? 2u : 0u}, type, out, size);
    case BUFFER_INFO_PIXELFORMAT:
        return info(INFO_DATATYPE_UINT64, uint64_t{progressive() ? 0x01100007u :
                    mode("packed") || mode("packed-odd") ? 0x010A0046u : 0x01080001u}, type, out, size);
    case BUFFER_INFO_PIXELFORMAT_NAMESPACE: return info(INFO_DATATYPE_UINT64, uint64_t{PIXELFORMAT_NAMESPACE_PFNC_32BIT}, type, out, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED);
    }
}
EXPORT(DSGetNumBufferParts)(DS_HANDLE, BUFFER_HANDLE, uint32_t *count)
{
    if (!multipart_payload()) return error(GC_ERR_NOT_IMPLEMENTED);
    *count = mode("multipart-count") ? 33 : mode("multipart-chunk-only") ? 0 : 2;
    return GC_ERR_SUCCESS;
}
EXPORT(DSGetBufferPartInfo)(DS_HANDLE, BUFFER_HANDLE, uint32_t index, BUFFER_PART_INFO_CMD cmd,
                            INFO_DATATYPE *type, void *out, size_t *size)
{
    switch (cmd) {
    case BUFFER_PART_INFO_BASE:
        return info(INFO_DATATYPE_PTR, static_cast<void *>(static_cast<unsigned char *>(announced_memory) +
                    (mode("multipart-bounds") ? 16 :
                     mode("multipart-gap") || mode("multipart-no-size") || multipart_chunk() ? index * 24 :
                     mode("multipart-overlap") ? index * 4 :
                     mode("multipart-duplicate") && completions > 1 ? 0 : index * 8)), type, out, size);
    case BUFFER_PART_INFO_DATA_SIZE: return info(INFO_DATATYPE_SIZET, size_t{8}, type, out, size);
    case BUFFER_PART_INFO_DATA_TYPE: return info(INFO_DATATYPE_SIZET, size_t{1}, type, out, size);
    case BUFFER_PART_INFO_DATA_FORMAT: return info(INFO_DATATYPE_UINT64, uint64_t{0x01080001}, type, out, size);
    case BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE: return info(INFO_DATATYPE_UINT64, uint64_t{PIXELFORMAT_NAMESPACE_PFNC_32BIT}, type, out, size);
    case BUFFER_PART_INFO_WIDTH: return info(INFO_DATATYPE_SIZET, size_t{4}, type, out, size);
    case BUFFER_PART_INFO_HEIGHT: return info(INFO_DATATYPE_SIZET, size_t{2}, type, out, size);
    default: return error(GC_ERR_NOT_IMPLEMENTED);
    }
}
EXPORT(DSGetBufferChunkData)(DS_HANDLE, BUFFER_HANDLE, SINGLE_CHUNK_DATA *chunks, size_t *count)
{
    if (mode("chunk-typed")) {
        const size_t available = completions == 1 ? 2 : 1;
        if (chunks) {
            if (*count < available) return error(GC_ERR_BUFFER_TOO_SMALL);
            chunks[0] = {0x1234, 16, 4};
            if (available == 2) chunks[1] = {0x1235, 20, 32};
        }
        *count = available;
        return GC_ERR_SUCCESS;
    }
    const bool duplicate = mode("chunk-duplicate");
    const bool overlap = mode("chunk-overlap");
    const size_t available = duplicate || overlap ? 2 : 1;
    if (!mode("chunk") && !mode("bad-chunk") && !chunk_data_size() && !multipart_chunk() && available == 1)
        return error(GC_ERR_NOT_IMPLEMENTED);
    if (chunks) {
        if (*count < available) return error(GC_ERR_BUFFER_TOO_SMALL);
        chunks[0] = {0x1234, mode("bad-chunk") ? -1 :
            mode("multipart-chunk-overlap") ? 24 : multipart_chunk() ? 40 : 8, 4};
        if (available == 2) chunks[1] = {duplicate ? 0x1234u : 0x1235u, overlap ? 10 : 12, 4};
    }
    *count = available;
    return GC_ERR_SUCCESS;
}
EXPORT(GCRegisterEvent)(EVENTSRC_HANDLE source, EVENT_TYPE type, EVENT_HANDLE *out)
{
    if (type == EVENT_MODULE || type == EVENT_REMOTE_DEVICE) {
        if (!module_events()) return error(GC_ERR_NOT_IMPLEMENTED);
        const auto slot = event_slot(source, type);
        if (!slot) return error(GC_ERR_INVALID_HANDLE);
        auto &queue = event_queues[slot];
        if (queue.active) return error(GC_ERR_RESOURCE_IN_USE);
        queue = {0, true};
        *out = handle(20 + slot);
        return GC_ERR_SUCCESS;
    }
    if (type == EVENT_FEATURE_INVALIDATE) {
        if (source != handle(3)) return error(GC_ERR_NOT_IMPLEMENTED);
        if (invalidation_registered) return error(GC_ERR_RESOURCE_IN_USE);
        invalidation_registered = true;
        invalidation_source = source;
        *out = handle(8);
        if (mode("invalidation-rollback")) return error(GC_ERR_IO);
        return GC_ERR_SUCCESS;
    }
    *out = mode("event-null") && !failures ? nullptr : handle(7);
    return GC_ERR_SUCCESS;
}
EXPORT(GCUnregisterEvent)(EVENTSRC_HANDLE source, EVENT_TYPE type)
{
    if (type == EVENT_MODULE || type == EVENT_REMOTE_DEVICE) {
        const auto slot = event_slot(source, type);
        if (!slot || !event_queues[slot].active) return error(GC_ERR_INVALID_HANDLE);
        if (mode("events-rollback") && failures++ == 0) return error(GC_ERR_BUSY);
        event_queues[slot].active = false;
    }
    if (type == EVENT_NEW_BUFFER && armed) std::abort();
    if (type == EVENT_NEW_BUFFER && mode("event-null") && !failures++) return error(GC_ERR_BUSY);
    if (type == EVENT_FEATURE_INVALIDATE) {
        if (!invalidation_registered || source != invalidation_source) return error(GC_ERR_INVALID_HANDLE);
        if (mode("invalidation-rollback") && failures++ == 0) return error(GC_ERR_BUSY);
        invalidation_registered = invalidation_pending = false;
        invalidation_source = nullptr;
    }
    return GC_ERR_SUCCESS;
}
EXPORT(EventGetData)(EVENT_HANDLE event_handle, void *out, size_t *size, uint64_t timeout)
{
    if (const auto slot = event_handle_slot(event_handle)) {
        if (timeout) std::abort();
        auto &queue = event_queues[slot];
        if (!queue.active) return error(GC_ERR_INVALID_HANDLE);
        if (mode("events-oversized") && queue.next == 0) {
            if (!out) { ++queue.next; return GC_ERR_SUCCESS; }
            *size = 1024 * 1024 + 1;
            return error(GC_ERR_BUFFER_TOO_SMALL);
        }
        if (!out) std::abort();
        if (queue.next == (slot == 4 ? 6u : 1u)) return error(GC_ERR_TIMEOUT);
        const size_t needed = slot == 4 && queue.next == 1 ? 512 : 8;
        if (*size < needed) {
            *size = needed;
            return error(GC_ERR_BUFFER_TOO_SMALL);
        }
        std::memset(out, 0, needed);
        static_cast<unsigned char *>(out)[0] = static_cast<unsigned char>(queue.next++);
        *size = needed;
        return GC_ERR_SUCCESS;
    }
    if (event_handle == handle(8)) {
        if (!invalidation_registered) return error(GC_ERR_INVALID_HANDLE);
        if (!invalidation_pending) return error(GC_ERR_TIMEOUT);
        if (mode("invalidation-oversized") && !failures) {
            if (out) {
                *size = 1024 * 1024 + 1;
                return error(GC_ERR_BUFFER_TOO_SMALL);
            }
            ++failures;
            invalidation_pending = false;
            return GC_ERR_SUCCESS;
        }
        const auto status = text("Width", static_cast<char *>(out), size);
        if (status == GC_ERR_SUCCESS) invalidation_pending = false;
        return status;
    }
    if (mode("io-event")) return error(GC_ERR_IO);
    if (mode("throw-event")) throw std::runtime_error("injected event exception");
    if (mode("abort")) return error(GC_ERR_ABORT);
    if (mode("progressive") && timeout) return error(GC_ERR_INVALID_PARAMETER);
    if ((!queued && !mode("multipart-duplicate")) || mode("timeout")) {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        return error(GC_ERR_TIMEOUT);
    }
    if (progressive()) {
        if (mode("progressive-controls") &&
            (!registers[88] || (registers[84] == 0 && completions)))
            return error(GC_ERR_TIMEOUT);
        if (mode("progressive-paced") && std::chrono::steady_clock::now() < next_progressive_frame)
            return error(GC_ERR_TIMEOUT);
        ++progressive_poll;
        const unsigned bytes = mode("progressive-complete") || progressive_poll >= 3 ? 16 :
                               progressive_poll == 1 ? 6 : 12;
        auto *image = announced_memory ? static_cast<unsigned char *>(announced_memory) : pixels.data();
        for (unsigned i = 0; i < bytes; ++i) image[i] = static_cast<unsigned char>(i + 1);
        if (bytes < 16) return error(GC_ERR_TIMEOUT);
    }
    EVENT_NEW_BUFFER_DATA event{handle(mode("event") ? 99 : 6), nullptr};
    std::memcpy(out, &event, sizeof(event));
    *size = sizeof(event);
    queued = false;
    delivered = true;
    ++completions;
    if (mode("chunk-typed")) {
        const uint32_t counter = 41 + completions;
        std::memcpy(static_cast<unsigned char *>(announced_memory) + 16, &counter, sizeof counter);
    }
    return 0;
}
EXPORT(EventGetDataInfo)(EVENT_HANDLE event, const void *data, size_t length,
                         EVENT_DATA_INFO_CMD cmd, INFO_DATATYPE *type, void *out, size_t *size)
{
    if (const auto slot = event_handle_slot(event)) {
        if (!event_queues[slot].active || length < 8) return error(GC_ERR_INVALID_PARAMETER);
        const auto index = static_cast<const unsigned char *>(data)[0];
        if (cmd == EVENT_DATA_NUMID)
            return index == 0 ? info(INFO_DATATYPE_UINT64, uint64_t{0x1234}, type, out, size)
                              : error(GC_ERR_NOT_AVAILABLE);
        if (cmd == EVENT_DATA_ID) {
            *type = INFO_DATATYPE_STRING;
            const char *id = index == 2 ? "112233445566778899" : index == 3 ? "abcd" :
                             index == 4 ? "NOTHEX" : "1234";
            return text(id, static_cast<char *>(out), size);
        }
        if (cmd == EVENT_DATA_VALUE) {
            if (index >= 3) return error(GC_ERR_NOT_AVAILABLE);
            const bool timestamp = mode("events-snapshot") && slot == 4 && index < 2;
            const size_t needed = index == 1 ? 520 : timestamp ? 12 : 4;
            if (*size < needed) {
                *size = needed;
                return error(GC_ERR_BUFFER_TOO_SMALL);
            }
            *type = INFO_DATATYPE_BUFFER;
            *size = needed;
            std::memset(out, 0, needed);
            const uint32_t value = static_cast<uint32_t>(slot * 10 + index);
            std::memcpy(out, &value, sizeof value);
            if (timestamp) {
                const uint64_t ticks = index ? UINT64_C(9007199254740993) : 0;
                std::memcpy(static_cast<unsigned char *>(out) + 4, &ticks, sizeof ticks);
            }
            return GC_ERR_SUCCESS;
        }
        return error(GC_ERR_NOT_IMPLEMENTED);
    }
    if (event != handle(8) || cmd != EVENT_DATA_ID || !length ||
        !std::memchr(data, 0, length)) return error(GC_ERR_INVALID_PARAMETER);
    *type = INFO_DATATYPE_STRING;
    const auto status = text(static_cast<const char *>(data), static_cast<char *>(out), size);
    if (mode("invalidation-malformed") && status == GC_ERR_SUCCESS) --*size;
    return status;
}
EXPORT(EventGetInfo)(EVENT_HANDLE event, EVENT_INFO_CMD cmd, INFO_DATATYPE *type, void *out, size_t *size)
{
    const auto slot = event_handle_slot(event);
    if (!slot || !event_queues[slot].active) return error(GC_ERR_INVALID_HANDLE);
    if (cmd != EVENT_SIZE_MAX && cmd != EVENT_INFO_DATA_SIZE_MAX) return error(GC_ERR_NOT_IMPLEMENTED);
    if (slot == 4 && !mode("events-rollback")) return error(GC_ERR_NOT_AVAILABLE);
    return info(INFO_DATATYPE_SIZET, size_t{mode("events-rollback") ? 2u * 1024 * 1024 : 16u},
                type, out, size);
}
EXPORT(EventKill)(EVENT_HANDLE) { return error(GC_ERR_NOT_IMPLEMENTED); }
constexpr char progressive_xml[] = R"xml(<RegisterDescription xmlns="http://www.genicam.org/GenApi/Version_1_1"
  ModelName="ProgressiveFixture" VendorName="Iris" StandardNameSpace="None"
  SchemaMajorVersion="1" SchemaMinorVersion="1" SchemaSubMinorVersion="0"
  MajorVersion="1" MinorVersion="0" SubMinorVersion="0"
  ProductGuid="c15886c4-f95c-44f6-8db9-3439e5a04cac" VersionGuid="14ccaba2-8fbe-4bff-a0a2-dc2242df430c">
  <Category Name="Root"><pFeature>Width</pFeature><pFeature>Height</pFeature><pFeature>PixelFormat</pFeature></Category>
  <Port Name="Device"/>
  <Integer Name="Width"><Value>2</Value></Integer>
  <Integer Name="Height"><Value>4</Value></Integer>
  <Enumeration Name="PixelFormat">
    <EnumEntry Name="Mono16"><Value>17825799</Value></EnumEntry><Value>17825799</Value>
  </Enumeration>
  <Enumeration Name="TriggerSelector">
    <EnumEntry Name="FrameStart"><pIsAvailable>FrameStartAvailable</pIsAvailable><Value>0</Value></EnumEntry>
    <EnumEntry Name="ExposureStart"><Value>1</Value></EnumEntry>
    <pValue>TriggerSelectorReg</pValue><pSelected>TriggerMode</pSelected><pSelected>TriggerSource</pSelected>
  </Enumeration>
  <Enumeration Name="TriggerMode">
    <EnumEntry Name="Off"><Value>0</Value></EnumEntry>
    <EnumEntry Name="On"><Value>1</Value></EnumEntry><pValue>TriggerModeReg</pValue>
  </Enumeration>
  <Enumeration Name="TriggerSource">
    <EnumEntry Name="Line0"><pIsAvailable>FrameStartSelected</pIsAvailable><Value>0</Value></EnumEntry>
    <EnumEntry Name="Line1"><Value>1</Value></EnumEntry>
    <pValue>TriggerSourceReg</pValue>
  </Enumeration>
  <Enumeration Name="AcquisitionMode">
    <EnumEntry Name="SingleFrame"><Value>0</Value></EnumEntry>
    <EnumEntry Name="Continuous"><Value>1</Value></EnumEntry><pValue>AcquisitionModeReg</pValue>
  </Enumeration>
  <Command Name="AcquisitionStart"><pValue>AcquisitionCommandReg</pValue><CommandValue>1</CommandValue></Command>
  <Command Name="AcquisitionStop"><pValue>AcquisitionCommandReg</pValue><CommandValue>0</CommandValue></Command>
  <IntSwissKnife Name="FrameStartSelected">
    <pVariable Name="SELECTOR">TriggerSelectorReg</pVariable><Formula>SELECTOR = 0</Formula>
  </IntSwissKnife>
  <IntReg Name="TriggerSelectorReg">
    <Address>64</Address><Length>4</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
    <Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="TriggerModeReg">
    <Address>68</Address><pIndex Offset="4">TriggerSelectorReg</pIndex><Length>4</Length>
    <AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="TriggerSourceReg">
    <Address>76</Address><pIndex Offset="4">TriggerSelectorReg</pIndex><Length>4</Length>
    <AccessMode>RW</AccessMode><pPort>Device</pPort><Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="AcquisitionModeReg">
    <Address>84</Address><Length>4</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
    <Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="AcquisitionCommandReg">
    <Address>88</Address><Length>4</Length><AccessMode>RW</AccessMode><pPort>Device</pPort>
    <Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
  <IntReg Name="FrameStartAvailable">
    <Address>96</Address><Length>4</Length><AccessMode>RO</AccessMode><pPort>Device</pPort>
    <Cachable>NoCache</Cachable><Endianess>LittleEndian</Endianess>
  </IntReg>
</RegisterDescription>)xml";

void local_xml_url(char (&url)[96], uint64_t address = xml_address)
{
    std::snprintf(url, sizeof(url), "local:node-fixture.xml;%llx;%zx",
                  static_cast<unsigned long long>(address),
                  progressive() ? sizeof(progressive_xml) - 1 : sizeof(node_xml) - 1);
}

EXPORT(GCGetNumPortURLs)(PORT_HANDLE port, uint32_t *count)
{
    if (!node_port(port) || (!mode("xml-indexed") && !mode("xml-file") && !mode("xml-legacy")))
        return error(GC_ERR_NOT_IMPLEMENTED);
    *count = mode("xml-indexed") ? 4 : 1;
    return GC_ERR_SUCCESS;
}

EXPORT(GCGetPortURLInfo)(PORT_HANDLE port, uint32_t index, URL_INFO_CMD cmd,
                        INFO_DATATYPE *type, void *out, size_t *size)
{
    if (!node_port(port) || (!mode("xml-indexed") && !mode("xml-file") && !mode("xml-legacy")))
        return error(GC_ERR_NOT_IMPLEMENTED);
    if (index >= (mode("xml-indexed") ? 4u : 1u)) return error(GC_ERR_INVALID_INDEX);
    if (cmd == URL_INFO_URL) {
        *type = INFO_DATATYPE_STRING;
        auto *str = static_cast<char *>(out);
        if (mode("xml-file")) {
            const char *url = std::getenv("IRIS_TEST_XML_URL");
            return url ? text(url, str, size) : error(GC_ERR_NOT_AVAILABLE);
        }
        if (!index) return text("http://example.com/node-fixture.xml", str, size);
        char url[96];
        local_xml_url(url, index == 3 ? xml_address : xml_address + index);
        return text(url, str, size);
    }
    if (index == 3) {
        switch (cmd) {
        case URL_INFO_SCHEMA_VER_MAJOR: case URL_INFO_SCHEMA_VER_MINOR: case URL_INFO_FILE_VER_MAJOR:
            return info(INFO_DATATYPE_INT32, int32_t{1}, type, out, size);
        case URL_INFO_FILE_VER_MINOR: case URL_INFO_FILE_VER_SUBMINOR:
            return info(INFO_DATATYPE_INT32, int32_t{0}, type, out, size);
        }
    }
    return error(GC_ERR_NOT_AVAILABLE);
}

EXPORT(GCGetPortURL)(PORT_HANDLE port, char *out, size_t *size)
{
    if (!node_port(port)) return text("unsupported:test", out, size);
    if (mode("xml-indexed") || mode("xml-file")) return error(GC_ERR_NOT_IMPLEMENTED);
    char url[96];
    local_xml_url(url);
    if (mode("xml-legacy")) {
        constexpr char first[] = "unsupported:first";
        const size_t used = sizeof(first) + std::strlen(url) + 2;
        if (!out || *size < used) {
            *size = used;
            return error(GC_ERR_BUFFER_TOO_SMALL);
        }
        std::memcpy(out, first, sizeof(first));
        std::memcpy(out + sizeof(first), url, used - sizeof(first) - 1);
        out[used - 1] = '\0';
        *size = used;
        return GC_ERR_SUCCESS;
    }
    return text(url, out, size);
}
EXPORT(GCGetPortInfo)(PORT_HANDLE port, PORT_INFO_CMD cmd, INFO_DATATYPE *type, void *out, size_t *size)
{
    if (mode("port-access") && port == handle(4)) {
        switch (cmd) {
        case PORT_INFO_ACCESS_READ: return info(INFO_DATATYPE_BOOL8, bool8_t{1}, type, out, size);
        case PORT_INFO_ACCESS_WRITE:
        case PORT_INFO_ACCESS_NA:
        case PORT_INFO_ACCESS_NI: return info(INFO_DATATYPE_BOOL8, bool8_t{0}, type, out, size);
        }
    }
    return error(GC_ERR_NOT_IMPLEMENTED);
}
EXPORT(GCReadPort)(PORT_HANDLE port, uint64_t address, void *out, size_t *size)
{
    if (!node_port(port)) return error(GC_ERR_IO);
    if (progressive() && address == xml_address && *size == sizeof(progressive_xml) - 1) {
        std::memcpy(out, progressive_xml, *size);
        return GC_ERR_SUCCESS;
    }
    if (address == xml_address && *size == sizeof(node_xml) - 1) {
        std::memcpy(out, node_xml, *size);
        return GC_ERR_SUCCESS;
    }
    if (mode("xml-indexed") && address == xml_address + 2 && *size == sizeof(node_xml) - 1) {
        std::memset(out, 'x', *size);
        return GC_ERR_SUCCESS;
    }
    if (address > registers.size() || *size > registers.size() - address)
        return error(GC_ERR_INVALID_ADDRESS);
    std::memcpy(out, registers.data() + address, *size);
    // The first status read observes the pending command, then the mock completes it.
    if (address == 32 && *size == 4 && registers[32]) {
        registers[32] = 0;
        ++registers[36];
    }
    return GC_ERR_SUCCESS;
}
EXPORT(GCWritePort)(PORT_HANDLE port, uint64_t address, const void *value, size_t *size)
{
    if (mode("port-access")) std::abort();
    if (!node_port(port)) return error(GC_ERR_IO);
    if (address > registers.size() || *size > registers.size() - address)
        return error(GC_ERR_INVALID_ADDRESS);
    std::memcpy(registers.data() + address, value, *size);
    ++scalar_writes;
    if (write_observer) write_observer(write_observer_context, address);
    // A producer-side change is intentionally absent from the GenApi dependency graph.
    if (address == 44 && *size == 4 && registers[44]) {
        registers[40] = registers[40] == 64 ? 48 : 64;
        invalidation_pending = invalidation_registered;
    }
    if (address == 56 && *size == 4 && registers[56])
        registers[52] = registers[52] == 64 ? 32 : 64;
    return GC_ERR_SUCCESS;
}

EXPORT(GCWritePortStacked)(PORT_HANDLE port, PORT_REGISTER_STACK_ENTRY *entries, size_t *count)
{
    ++stacked_calls;
    if (!node_port(port) || mode("stacking-unsupported")) return error(GC_ERR_NOT_IMPLEMENTED);
    if (mode("stacking-throw")) throw std::runtime_error("injected stacked write exception");
    const auto requested = *count;
    *count = 0;
    for (; *count < requested; ++*count) {
        if (*count == 2 && mode("stacking-partial")) return error(GC_ERR_IO);
        if (*count == 2 && mode("stacking-short")) return GC_ERR_SUCCESS;
        const auto &entry = entries[*count];
        if (entry.Address > registers.size() || entry.Size > registers.size() - entry.Address)
            return error(GC_ERR_INVALID_ADDRESS);
        std::memcpy(registers.data() + entry.Address, entry.pBuffer, entry.Size);
    }
    return GC_ERR_SUCCESS;
}
