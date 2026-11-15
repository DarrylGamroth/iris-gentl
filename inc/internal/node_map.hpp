#pragma once

#include "iris.h"

#include <GenApi/GenApi.h>
#include <GenApi/EventAdapterGeneric.h>
#include <GenTL.h>

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace iris {

inline constexpr size_t max_node_map_xml_bytes = 64 * 1024 * 1024;

struct Producer;

struct NodeMapEvents {
    explicit NodeMapEvents(GenApi::INodeMap *map) : adapter(map) {}

    std::vector<uint8_t> envelope;
    std::vector<uint8_t> data;
    GenApi::CEventAdapterGeneric adapter;
    GenTL::EVENT_HANDLE handle = nullptr;
    bool ready = false;
};

struct Port : GenApi::IPortStacked {
    Producer &producer;
    GenTL::PORT_HANDLE handle;
    GenApi::EAccessMode access;
    size_t *stacked_written = nullptr;

    Port(Producer &p, GenTL::PORT_HANDLE port, bool writable);
    GenApi::EAccessMode GetAccessMode() const override;
    void Read(void *data, int64_t address, int64_t length) override;
    void Write(const void *data, int64_t address, int64_t length) override;
    void Write(GenApi::PORT_REGISTER_STACK_ENTRY *entries, size_t count) override;

private:
    template<class F>
    void transfer(int64_t address, int64_t length, F f, const char *op);
};

} // namespace iris

struct iris_node_map {
    struct DestroyStack {
        void operator()(GenApi::CNodeWriteConcatenator *stack) const { stack->Destroy(); }
    };
    iris::Port port;
    GenApi::CNodeMapRef nodes;
    std::unique_ptr<GenApi::CNodeWriteConcatenator, DestroyStack> write_stack;
    std::mutex *mutex;
    GenTL::EVENT_HANDLE invalidation = nullptr;
    GenTL::EVENTSRC_HANDLE invalidation_source = nullptr;
    bool invalidation_ready = false;
    GenTL::EVENTSRC_HANDLE event_source = nullptr;
    std::unique_ptr<iris::NodeMapEvents> events;
    GenTL::EVENT_TYPE event_type = GenTL::EVENT_MODULE;
    std::string xml_url;
    std::string xml_source;
    bool xml_zipped = false;
    iris_node_map_xml_info_t xml_info{};
    iris_node_map(iris::Producer &p, GenTL::PORT_HANDLE h, bool writable, std::mutex &lock)
        : port(p, h, writable), mutex(&lock) {}
};

namespace iris {
std::string unzip_xml(std::string_view zip, const char *operation);
std::unique_ptr<iris_node_map> load_node_map(Producer &producer, GenTL::PORT_HANDLE port,
                                            bool writable, std::mutex &mutex, const char *xml_path = nullptr);
std::unique_ptr<iris_node_map> load_remote_node_map(iris_device &device, const char *xml_path = nullptr);
iris_status_t disable_node_map_events(iris_node_map *map);
iris_status_t receive_event_envelope(Producer &producer, GenTL::EVENT_HANDLE event,
                                     std::vector<uint8_t> &data, size_t &size, size_t *processed = nullptr);
}
