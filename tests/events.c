#define _POSIX_C_SOURCE 200809L
#include "iris.h"
#include "allocations.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return;
    iris_error_t error;
    iris_last_error(&error);
    fprintf(stderr, "%s returned %d: %s: %s\n", call, status, error.operation, error.message);
    exit(1);
}
#define OK(x) check((x), #x)

static void expect_counter(iris_node_map_t *map, int64_t expected)
{
    int64_t counter;
    OK(iris_feature_get_integer(map, "EventCounter", &counter));
    EXPECT(counter == expected);
}

static void expect_event(iris_node_map_t *map, size_t size)
{
    iris_node_map_event_t event;
    OK(iris_node_map_try_get_event(map, &event));
    EXPECT(event.has_numeric_id && event.numeric_id == 0x1234 && strcmp(event.id, "1234") == 0);
    EXPECT(event.data_size == size);
}

static void expect_snapshot_value(const iris_event_snapshot_t *snapshot, const char *name, const char *expected)
{
    char text[32] = "untouched";
    size_t size = 0;
    OK(iris_event_snapshot_feature_to_string(snapshot, name, NULL, &size));
    EXPECT(size == strlen(expected) + 1 && size <= sizeof text);
    size_t small = 1;
    EXPECT(iris_event_snapshot_feature_to_string(snapshot, name, text, &small) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(small == size && strcmp(text, "untouched") == 0);
    OK(iris_event_snapshot_feature_to_string(snapshot, name, text, &size));
    EXPECT(strcmp(text, expected) == 0);
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    EXPECT(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + now.tv_nsec;
}

static void expect_snapshot(const iris_event_snapshot_t *snapshot, uint32_t counter,
                            size_t data_size, uint64_t device_time)
{
    iris_event_snapshot_info_t info;
    OK(iris_event_snapshot_info(snapshot, &info));
    EXPECT(info.event.has_numeric_id && info.event.numeric_id == 0x1234);
    EXPECT(info.event.data_size == data_size && info.data);
    EXPECT((info.valid & IRIS_EVENT_HAS_HOST_TIMESTAMP_NS) != 0 && info.host_timestamp_ns > 0);
    EXPECT((info.valid & IRIS_EVENT_HAS_DEVICE_TIMESTAMP) != 0 && info.device_timestamp == device_time);
    uint32_t raw;
    memcpy(&raw, info.data, sizeof raw);
    EXPECT(raw == counter);
    char text[16];
    snprintf(text, sizeof text, "%u", counter);
    expect_snapshot_value(snapshot, "EventCounter", text);
    const iris_feature_list_t *features = iris_event_snapshot_features(snapshot);
    EXPECT(iris_feature_list_count(features) == 2);
    iris_feature_entry_t entry;
    OK(iris_feature_list_get(features, 0, &entry));
    EXPECT(strcmp(entry.name, "EventCounter") == 0 && entry.info.type == IRIS_FEATURE_INTEGER);
    OK(iris_feature_list_get(features, 1, &entry));
    EXPECT(strcmp(entry.name, "EventExposureEndTimestamp") == 0);
    size_t size = 0;
    EXPECT(iris_event_snapshot_feature_to_string(snapshot, "CombinedCounter", NULL, &size) == IRIS_NOT_FOUND);
    EXPECT(iris_event_snapshot_feature_to_string(snapshot, "Width", NULL, &size) == IRIS_NOT_FOUND);
}

static iris_event_snapshot_t *test_snapshots(iris_node_map_t *remote)
{
    iris_event_snapshot_t *first = NULL, *next = NULL;
    EXPECT(iris_node_map_try_get_event_snapshot(remote, &first) == IRIS_INVALID_STATE && !first);
    EXPECT(!iris_event_snapshot_features(NULL));
    OK(iris_node_map_enable_events(remote, 1));
    const uint64_t before = monotonic_ns();
    OK(iris_node_map_try_get_event_snapshot(remote, &first));
    const uint64_t after = monotonic_ns();
    iris_event_snapshot_info_t info;
    OK(iris_event_snapshot_info(first, &info));
    EXPECT(info.host_timestamp_ns >= before && info.host_timestamp_ns <= after);
    expect_snapshot(first, 40, 12, 0);
    OK(iris_node_map_try_get_event_snapshot(remote, &next));
    expect_snapshot(next, 41, 520, UINT64_C(9007199254740993));
    expect_snapshot(first, 40, 12, 0);
    expect_counter(remote, 41);
    iris_event_snapshot_close(&next);
    EXPECT(!next);

    OK(iris_node_map_try_get_event_snapshot(remote, &next));
    OK(iris_event_snapshot_info(next, &info));
    EXPECT(strcmp(info.event.id, "112233445566778899") == 0 && !info.event.has_numeric_id);
    EXPECT(info.event.data_size == 4 && !(info.valid & IRIS_EVENT_HAS_DEVICE_TIMESTAMP));
    expect_snapshot_value(next, "EventWideCounter", "42");
    size_t size = 0;
    EXPECT(iris_event_snapshot_feature_to_string(next, "EventCounter", NULL, &size) == IRIS_NOT_FOUND);
    iris_event_snapshot_close(&next);

    OK(iris_node_map_try_get_event_snapshot(remote, &next));
    OK(iris_event_snapshot_info(next, &info));
    EXPECT(strcmp(info.event.id, "abcd") == 0 && !info.event.data_size && !info.data);
    EXPECT(iris_feature_list_count(iris_event_snapshot_features(next)) == 0);
    iris_event_snapshot_close(&next);
    EXPECT(iris_node_map_try_get_event_snapshot(remote, &next) == IRIS_PRODUCER_ERROR && !next);
    OK(iris_node_map_try_get_event_snapshot(remote, &next));
    OK(iris_event_snapshot_info(next, &info));
    EXPECT(!info.event.data_size && !info.data && !(info.valid & IRIS_EVENT_HAS_DEVICE_TIMESTAMP));
    size = 0;
    EXPECT(iris_event_snapshot_feature_to_string(next, "EventCounter", NULL, &size) < 0);
    iris_event_snapshot_close(&next);
    EXPECT(iris_node_map_try_get_event_snapshot(remote, &next) == IRIS_NO_EVENT && !next);
    OK(iris_node_map_enable_events(remote, 0));
    return first;
}

static void test_capture(iris_stream_t *stream)
{
    _Alignas(64) unsigned char memory[64] = {0};
    const iris_buffer_t buffer = {memory, sizeof memory, NULL};
    const iris_start_options_t options = {1, IRIS_ACQUISITION_STREAM_ONLY, &buffer};
    OK(iris_stream_start(stream, &options));
    iris_frame_t frame;
    allocation_begin();
    OK(iris_stream_try_get_frame(stream, &frame));
    EXPECT(frame.status == IRIS_FRAME_COMPLETE && frame.data == memory);
    OK(iris_stream_release_frame(stream, &frame));
    const allocation_counts counts = allocation_end();
    EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
    OK(iris_stream_stop(stream));
}

static void test_events(iris_device_t *device, iris_node_map_t *remote)
{
    iris_node_map_event_t event;
    EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_INVALID_STATE);
    EXPECT(iris_node_map_enable_events(remote, 2) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_node_map_try_get_event(NULL, &event) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_node_map_try_get_event(remote, NULL) == IRIS_INVALID_ARGUMENT);
    OK(iris_node_map_enable_events(remote, 1));
    OK(iris_node_map_enable_events(remote, 1));
    expect_event(remote, 4);
    expect_counter(remote, 40);

    for (iris_module_t module = IRIS_SYSTEM_MODULE; module <= IRIS_DEVICE_MODULE; ++module) {
        iris_node_map_t *map;
        OK(iris_device_node_map(device, module, &map));
        OK(iris_node_map_enable_events(map, 1));
        expect_event(map, 4);
        expect_counter(map, module * 10);
        EXPECT(iris_node_map_try_get_event(map, &event) == IRIS_NO_EVENT);
        EXPECT(!event.id[0] && !event.data_size);
    }
    expect_counter(remote, 40);
    expect_event(remote, 520);
    expect_counter(remote, 41);
    OK(iris_node_map_try_get_event(remote, &event));
    EXPECT(strcmp(event.id, "112233445566778899") == 0 && !event.has_numeric_id && event.data_size == 4);
    int64_t counter;
    OK(iris_feature_get_integer(remote, "EventWideCounter", &counter));
    EXPECT(counter == 42);
    OK(iris_node_map_try_get_event(remote, &event));
    EXPECT(strcmp(event.id, "abcd") == 0 && event.numeric_id == 0xabcd && !event.data_size);
    expect_counter(remote, 41);
    EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_PRODUCER_ERROR);
    EXPECT(!event.id[0] && !event.has_numeric_id && !event.data_size);
    expect_event(remote, 0);
    EXPECT(iris_feature_get_integer(remote, "EventCounter", &counter) < 0);
    EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_NO_EVENT);
    OK(iris_node_map_enable_events(remote, 0));
    OK(iris_node_map_enable_events(remote, 0));
    EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_INVALID_STATE);
    OK(iris_node_map_enable_events(remote, 1));
    expect_event(remote, 4);
    expect_counter(remote, 40);

    iris_stream_t *stream = NULL;
    iris_node_map_t *map;
    OK(iris_stream_open(device, 0, &stream));
    OK(iris_stream_node_map(stream, &map));
    OK(iris_node_map_enable_events(map, 1));
    test_capture(stream);
    iris_event_snapshot_t *snapshot = NULL;
    OK(iris_node_map_try_get_event_snapshot(map, &snapshot));
    expect_counter(map, 50);
    OK(iris_stream_close(&stream));
    expect_snapshot_value(snapshot, "EventCounter", "50");
    size_t size = 0;
    const iris_status_t short_read =
        iris_event_snapshot_feature_to_string(snapshot, "EventExposureEndTimestamp", NULL, &size);
    EXPECT(short_read < 0 && short_read != IRIS_NOT_FOUND);
    iris_event_snapshot_close(&snapshot);
}

static void run(const char *cti, const char *scenario)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *remote;
    iris_event_snapshot_t *snapshot = NULL;
    const iris_device_selector_t selector = {cti, "test-interface", "test-device", NULL};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote));
    if (strcmp(scenario, "events") == 0) {
        test_events(device, remote);
    } else if (strcmp(scenario, "events-snapshot") == 0) {
        snapshot = test_snapshots(remote);
    } else if (strcmp(scenario, "events-rollback") == 0) {
        EXPECT(iris_node_map_enable_events(remote, 1) == IRIS_OUT_OF_RANGE);
        iris_error_t error;
        iris_last_error(&error);
        EXPECT(strcmp(error.operation, "EventGetInfo") == 0);
        EXPECT(iris_node_map_enable_events(remote, 1) == IRIS_INVALID_STATE);
        iris_node_map_event_t event;
        EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_INVALID_STATE);
    } else if (strcmp(scenario, "events-oversized") == 0) {
        OK(iris_node_map_enable_events(remote, 1));
        iris_node_map_event_t event;
        EXPECT(iris_node_map_try_get_event(remote, &event) == IRIS_OUT_OF_RANGE);
        EXPECT(!event.id[0]);
        expect_event(remote, 520);
        expect_counter(remote, 41);
    } else {
        EXPECT(iris_node_map_enable_events(remote, 1) == IRIS_NOT_SUPPORTED);
    }
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
    if (snapshot) {
        expect_snapshot(snapshot, 40, 12, 0);
        iris_event_snapshot_close(&snapshot);
    }
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    run(argv[1], "events");
    run(argv[1], "events-snapshot");
    run(argv[1], "events-rollback");
    run(argv[1], "events-oversized");
    run(argv[1], "nodes");
}
