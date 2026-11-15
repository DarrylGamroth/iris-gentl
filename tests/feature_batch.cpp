#include "node_map.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <thread>

#include <dlfcn.h>

#define EXPECT(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void expect_enum(iris_node_map_t *map, const char *name, const char *expected)
{
    char value[32];
    size_t size = sizeof(value);
    EXPECT(iris_feature_get_enumeration(map, name, value, &size) == IRIS_OK);
    EXPECT(std::strcmp(value, expected) == 0);
}

static void test_order_and_failure(iris_node_map_t *map)
{
    size_t applied = 99;
    EXPECT(iris_feature_write_batch(map, nullptr, 0, &applied) == IRIS_OK && applied == 0);
    EXPECT(iris_feature_write_batch(map, nullptr, 1, &applied) == IRIS_INVALID_ARGUMENT && applied == 0);
    EXPECT(iris_feature_write_batch(nullptr, nullptr, 0, &applied) == IRIS_INVALID_ARGUMENT && applied == 0);
    EXPECT(iris_feature_write_batch(map, nullptr, 0, nullptr) == IRIS_INVALID_ARGUMENT);

    const iris_feature_write_t writes[] = {
        {"TriggerSelector", "FrameStart"}, {"TriggerMode", "On"},
        {"TriggerSelector", "ExposureStart"}, {"TriggerMode", "Off"},
    };
    EXPECT(iris_feature_write_batch(map, writes, std::size(writes), &applied) == IRIS_OK);
    EXPECT(applied == std::size(writes));
    expect_enum(map, "TriggerSelector", "ExposureStart");
    expect_enum(map, "TriggerMode", "Off");
    EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "FrameStart") == IRIS_OK);
    expect_enum(map, "TriggerMode", "On");

    int64_t initial_width;
    EXPECT(iris_feature_get_integer(map, "Width", &initial_width) == IRIS_OK);
    const iris_feature_write_t bad_width[] = {
        {"TriggerMode", "Off"}, {"Width", "80"}, {"TriggerSelector", "ExposureStart"},
    };
    EXPECT(iris_feature_write_batch(map, bad_width, std::size(bad_width), &applied) == IRIS_OUT_OF_RANGE);
    EXPECT(applied == 1);
    iris_error_t error;
    iris_last_error(&error);
    EXPECT(std::strcmp(error.operation, "write feature batch") == 0 && *error.message);
    expect_enum(map, "TriggerSelector", "FrameStart");
    expect_enum(map, "TriggerMode", "Off");
    int64_t width;
    EXPECT(iris_feature_get_integer(map, "Width", &width) == IRIS_OK && width == initial_width);

    const iris_feature_write_t invalid_write{"TriggerMode", nullptr};
    EXPECT(iris_feature_write_batch(map, &invalid_write, 1, &applied) == IRIS_INVALID_ARGUMENT && applied == 0);
    const iris_feature_write_t command{"TriggerSoftware", "Execute"};
    EXPECT(iris_feature_write_batch(map, &command, 1, &applied) == IRIS_WRONG_TYPE && applied == 0);
}

struct RegisterWrites {
    std::promise<void> selector_written;
    std::promise<void> contender_ready;
    std::atomic<size_t> count{0};
    std::array<uint64_t, 3> addresses{};
};

static void observe_write(void *context, uint64_t address)
{
    auto &writes = *static_cast<RegisterWrites *>(context);
    const auto index = writes.count.fetch_add(1);
    EXPECT(index < writes.addresses.size());
    writes.addresses[index] = address;
    if (index == 0) {
        writes.selector_written.set_value();
        writes.contender_ready.get_future().wait();
    }
}

static void test_contention(iris_node_map_t *map, const char *cti)
{
    void *library = dlopen(cti, RTLD_NOW | RTLD_NOLOAD);
    EXPECT(library);
    using SetObserver = void (*)(void (*)(void *, uint64_t), void *);
    auto set_observer = reinterpret_cast<SetObserver>(dlsym(library, "iris_test_observe_writes"));
    EXPECT(set_observer);
    EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "ExposureStart") == IRIS_OK);
    RegisterWrites writes;
    set_observer(observe_write, &writes);
    std::thread contender([&] {
        writes.selector_written.get_future().wait();
        const bool acquired = map->mutex->try_lock();
        if (acquired) map->mutex->unlock();
        EXPECT(!acquired);
        iris_node_map_event_t event{};
        EXPECT(iris_node_map_try_get_event(map, &event) == IRIS_BUSY);
        iris_event_snapshot_t *snapshot = nullptr;
        EXPECT(iris_node_map_try_get_event_snapshot(map, &snapshot) == IRIS_BUSY && !snapshot);
        writes.contender_ready.set_value();
        EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "ExposureStart") == IRIS_OK);
    });
    const iris_feature_write_t batch[] = {{"TriggerSelector", "FrameStart"}, {"TriggerMode", "On"}};
    size_t applied;
    EXPECT(iris_feature_write_batch(map, batch, std::size(batch), &applied) == IRIS_OK && applied == 2);
    contender.join();
    set_observer(nullptr, nullptr);
    EXPECT(dlclose(library) == 0);

    const std::array<uint64_t, 3> expected{0, 4, 0};
    EXPECT(writes.count == expected.size() && writes.addresses == expected);
    expect_enum(map, "TriggerSelector", "ExposureStart");
    expect_enum(map, "TriggerMode", "Off");
    EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "FrameStart") == IRIS_OK);
    expect_enum(map, "TriggerMode", "On");
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2 && setenv("IRIS_FAULT", "nodes", 1) == 0);
    iris_context_t *context = nullptr;
    iris_device_t *device = nullptr;
    iris_node_map_t *map = nullptr;
    const iris_device_selector_t selector{argv[1], "test-interface", "test-device", nullptr};
    EXPECT(iris_context_create(&context) == IRIS_OK);
    EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_OK);
    EXPECT(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map) == IRIS_OK);
    test_order_and_failure(map);
    test_contention(map, argv[1]);
    EXPECT(iris_device_close(&device) == IRIS_OK);
    EXPECT(iris_context_close(&context) == IRIS_OK);
}
