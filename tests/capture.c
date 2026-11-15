#define _POSIX_C_SOURCE 200809L
#include "iris.h"
#include "allocations.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    iris_device_t *device;
    iris_stream_t *stream;
    iris_node_map_t *remote;
    uint64_t width, height;
    uint64_t complete;
    uint64_t first_id, last_id, id_gaps, last_host, max_interval_ns;
    int have_id;
    unsigned rejected;
    int allow_rejected;
    int software_trigger;
    const iris_buffer_t *buffers;
} camera_t;

static void check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return;
    iris_error_t error;
    iris_last_error(&error);
    fprintf(stderr, "%s -> %d: %s: %s (native %d)\n",
            call, status, error.operation, error.message, error.native_code);
    exit(1);
}
#define CHECK(call) check((call), #call)
#define EXPECT(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

enum { CALLER_BUFFER_COUNT = 8 };

static void start_with_buffers(camera_t *camera, iris_buffer_t *buffers,
                               iris_acquisition_control_t control)
{
    iris_buffer_requirements_t requirements;
    CHECK(iris_stream_buffer_requirements(camera->stream, &requirements));
    EXPECT(requirements.minimum_buffer_count <= CALLER_BUFFER_COUNT);
    const size_t alignment = requirements.alignment > 64 ? requirements.alignment : 64;
    for (size_t i = 0; i < CALLER_BUFFER_COUNT; ++i) {
        EXPECT(posix_memalign(&buffers[i].data, alignment, requirements.payload_size) == 0);
        buffers[i].size = requirements.payload_size;
        buffers[i].user_data = &buffers[i];
    }
    const iris_start_options_t options = {CALLER_BUFFER_COUNT, control, buffers};
    CHECK(iris_stream_start(camera->stream, &options));
    camera->software_trigger = control == IRIS_ACQUISITION_STREAM_ONLY;
    camera->buffers = buffers;
}

static void free_buffers(iris_buffer_t *buffers)
{
    for (size_t i = 0; i < CALLER_BUFFER_COUNT; ++i) free(buffers[i].data);
}

static void open_camera(iris_context_t *context, const char *path, const char *serial,
                        const char *xml_path, camera_t *camera)
{
    iris_device_list_t *list = NULL;
    CHECK(iris_discover(context, path, 1000, &list));
    const size_t count = iris_device_list_count(list);
    iris_device_info_t chosen = {0};
    size_t matches = 0;
    for (size_t i = 0; i < count; ++i) {
        iris_device_info_t info;
        CHECK(iris_device_list_get(list, i, &info));
        printf("%s %s %s %s\n", info.interface_id, info.device_id,
               info.serial_number ? info.serial_number : "-", info.model ? info.model : "-");
        if (!serial || (info.serial_number && strcmp(serial, info.serial_number) == 0)) {
            chosen = info;
            ++matches;
        }
    }
    fflush(stdout);
    EXPECT(matches == 1);
    iris_device_selector_t selector = {chosen.producer_path, chosen.interface_id, chosen.device_id, NULL};
    const iris_device_open_options_t options = {1000, xml_path, IRIS_DEVICE_CONTROL};
    CHECK(iris_device_open_with_options(context, &selector, &options, &camera->device));
    iris_device_list_close(&list);
    size_t streams;
    CHECK(iris_device_stream_count(camera->device, &streams));
    EXPECT(streams > 0);
    CHECK(iris_stream_open(camera->device, 0, &camera->stream));
    CHECK(iris_device_node_map(camera->device, IRIS_REMOTE_DEVICE, &camera->remote));
    int64_t width, height;
    CHECK(iris_feature_get_integer(camera->remote, "Width", &width));
    CHECK(iris_feature_get_integer(camera->remote, "Height", &height));
    EXPECT(width > 0 && height > 0);
    char pixel_format[64] = "";
    size_t format_size = sizeof(pixel_format);
    const iris_status_t format_status =
        iris_feature_get_enumeration(camera->remote, "PixelFormat", pixel_format, &format_size);
    printf("geometry %lld x %lld, pixel format %s\n", (long long)width,
           (long long)height, format_status == IRIS_OK ? pixel_format : "unavailable");
    camera->have_id = 0;
    camera->last_host = 0;
    camera->max_interval_ns = 0;
    camera->id_gaps = 0;
    camera->buffers = NULL;
    camera->width = width;
    camera->height = height;
}

static void start_mock(camera_t *camera)
{
    const iris_start_options_t options = {0, IRIS_ACQUISITION_STREAM_ONLY, NULL};
    CHECK(iris_stream_start(camera->stream, &options));
    camera->software_trigger = 1;
}

static void test_features(camera_t *a, camera_t *b)
{
    iris_node_map_t *maps[] = {a->remote, b->remote};
    for (size_t i = 0; i < sizeof(maps) / sizeof(*maps); ++i) {
        iris_feature_list_t *features = NULL;
        CHECK(iris_node_map_browse(maps[i], NULL, IRIS_CATEGORY_FEATURES, &features));
        EXPECT(iris_feature_list_count(features) > 0);
        iris_feature_list_close(&features);
    }
    iris_feature_info_t info;
    iris_integer_range_t integers;
    iris_float_range_t floats;
    char text[128];
    size_t size = 0, count = 0;
    CHECK(iris_feature_info(a->remote, "Width", &info));
    EXPECT(info.type == IRIS_FEATURE_INTEGER);
    CHECK(iris_feature_integer_range(a->remote, "Width", &integers));
    const int64_t original_width = (int64_t)a->width;
    const int64_t changed_width = integers.minimum;
    EXPECT(changed_width != original_width);
    CHECK(iris_feature_set_integer(a->remote, "Width", changed_width));
    int64_t observed;
    CHECK(iris_feature_get_integer(a->remote, "Width", &observed));
    EXPECT(observed == changed_width);
    CHECK(iris_feature_get_integer(a->remote, "PayloadSize", &observed));
    EXPECT(observed == changed_width * (int64_t)a->height * 2);
    CHECK(iris_feature_set_integer(a->remote, "Width", original_width));
    CHECK(iris_feature_to_string(a->remote, "Width", NULL, &size));
    size = 1;
    EXPECT(iris_feature_to_string(a->remote, "Width", text, &size) == IRIS_BUFFER_TOO_SMALL);
    CHECK(iris_feature_to_string(a->remote, "Width", text, &size));
    CHECK(iris_feature_from_string(a->remote, "Width", text));
    EXPECT(iris_feature_integer_values(a->remote, "Width", NULL, &count) == IRIS_NOT_SUPPORTED);
    EXPECT(iris_feature_get_float(a->remote, "Width", &floats.minimum) == IRIS_WRONG_TYPE);
    EXPECT(iris_feature_info(a->remote, "NoSuchFeature", &info) == IRIS_NOT_FOUND);
    CHECK(iris_feature_float_range(b->remote, "ExposureTime", &floats));
    CHECK(iris_feature_get_float(b->remote, "ExposureTime", &floats.increment));
    EXPECT(iris_feature_float_values(b->remote, "ExposureTime", NULL, &count) == IRIS_NOT_SUPPORTED);
    size = sizeof(text);
    CHECK(iris_feature_unit(b->remote, "ExposureTime", text, &size));
    EXPECT(strcmp(text, "us") == 0);
    size = sizeof(text);
    CHECK(iris_feature_get_enumeration(b->remote, "AcquisitionMode", text, &size));
    CHECK(iris_feature_enumeration_count(b->remote, "AcquisitionMode", &count));
    EXPECT(count >= 2);
    for (size_t i = 0; i < count; ++i) {
        iris_feature_access_t access;
        size = sizeof(text);
        CHECK(iris_feature_enumeration_entry(b->remote, "AcquisitionMode", i, text, &size, &access));
        EXPECT(size > 1);
    }
    iris_feature_access_t access;
    EXPECT(iris_feature_enumeration_entry(b->remote, "AcquisitionMode", count, text, &size, &access)
           == IRIS_INVALID_ARGUMENT);
    iris_node_map_t *map;
    CHECK(iris_stream_node_map(b->stream, &map));
    CHECK(iris_device_node_map(b->device, IRIS_SYSTEM_MODULE, &map));
    CHECK(iris_device_node_map(b->device, IRIS_INTERFACE_MODULE, &map));
    CHECK(iris_device_node_map(b->device, IRIS_DEVICE_MODULE, &map));
    size = sizeof(text);
    CHECK(iris_feature_get_string(b->remote, "DeviceSerialNumber", text, &size));
    EXPECT(strcmp(text, "test-serial") == 0);
    EXPECT(iris_feature_set_string(b->remote, "DeviceSerialNumber", "ignored") < 0);
    uint32_t flag;
    CHECK(iris_feature_get_boolean(b->remote, "AcquisitionStatus", &flag));
    EXPECT(!flag);
    EXPECT(iris_feature_set_boolean(b->remote, "AcquisitionStatus", 2) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_feature_set_boolean(b->remote, "AcquisitionStatus", 1) < 0);
}

static void test_simulator_write_stacks(camera_t *triggered, camera_t *continuous)
{
    iris_integer_range_t range;
    CHECK(iris_feature_integer_range(triggered->remote, "Width", &range));
    char width[32];
    snprintf(width, sizeof(width), "%lld", (long long)range.minimum);
    CHECK(iris_node_map_start_stacking(triggered->remote));
    CHECK(iris_node_map_stack_add(triggered->remote, "Width", width));
    CHECK(iris_node_map_start_stacking(continuous->remote));
    CHECK(iris_node_map_stack_add(continuous->remote, "ExposureTime", "2500"));
    CHECK(iris_node_map_stack_add(continuous->remote, "AcquisitionMode", "Continuous"));

    size_t triggered_writes, continuous_writes;
    CHECK(iris_node_map_write_stack(continuous->remote, &continuous_writes));
    double exposure;
    CHECK(iris_feature_get_float(continuous->remote, "ExposureTime", &exposure));
    EXPECT(exposure > 2499.0 && exposure < 2501.0 && continuous_writes > 0);
    char mode[32];
    size_t size = sizeof(mode);
    CHECK(iris_feature_get_enumeration(continuous->remote, "AcquisitionMode", mode, &size));
    EXPECT(strcmp(mode, "Continuous") == 0);

    int64_t observed_width;
    CHECK(iris_feature_get_integer(triggered->remote, "Width", &observed_width));
    EXPECT(observed_width == (int64_t)triggered->width);
    CHECK(iris_node_map_write_stack(triggered->remote, &triggered_writes));
    CHECK(iris_feature_get_integer(triggered->remote, "Width", &observed_width));
    EXPECT(observed_width == range.minimum && triggered_writes > 0);
    CHECK(iris_feature_set_integer(triggered->remote, "Width", (int64_t)triggered->width));
    CHECK(iris_feature_set_float(continuous->remote, "ExposureTime", 2000));
    printf("stacked simulator register writes: triggered %zu, continuous %zu\n", triggered_writes, continuous_writes);
}

typedef struct { iris_stream_t *stream; iris_status_t result; } waiter_t;
static void *wait_for_frame(void *opaque)
{
    waiter_t *waiter = opaque;
    iris_frame_t frame;
    waiter->result = iris_stream_wait_frame(waiter->stream, 1000, &frame);
    if (waiter->result == IRIS_OK) CHECK(iris_stream_release_frame(waiter->stream, &frame));
    return NULL;
}

static void test_wait(camera_t *a)
{
    iris_frame_t frame;
    allocation_begin();
    iris_status_t status = iris_stream_wait_frame(a->stream, 2, &frame);
    if (status == IRIS_OK) CHECK(iris_stream_release_frame(a->stream, &frame));
    else EXPECT(status == IRIS_TIMEOUT);
    EXPECT(allocation_end().consumer_calls == 0);
    waiter_t waiter = {a->stream, IRIS_OK};
    pthread_t thread;
    EXPECT(pthread_create(&thread, NULL, wait_for_frame, &waiter) == 0);
    const struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
    CHECK(iris_stream_stop(a->stream));
    EXPECT(pthread_join(thread, NULL) == 0);
    EXPECT(waiter.result == IRIS_STOPPED || waiter.result == IRIS_OK);
    EXPECT(iris_stream_try_get_frame(a->stream, &frame) == IRIS_STOPPED);
    start_mock(a);
    CHECK(iris_feature_execute(a->remote, "TriggerSoftware"));
    uint32_t done;
    EXPECT(iris_feature_command_done(a->remote, "TriggerSoftware", &done) == IRIS_PRODUCER_ERROR);
    CHECK(iris_stream_wait_frame(a->stream, 1000, &frame));
    EXPECT(frame.status == IRIS_FRAME_COMPLETE);
    iris_stream_stats_t stats;
    CHECK(iris_stream_stats(a->stream, &stats));
    EXPECT(stats.outstanding_frames == 1);
    CHECK(iris_stream_stop(a->stream));
    EXPECT(iris_stream_close(&a->stream) == IRIS_BUSY);
    iris_frame_t stale = frame;
    CHECK(iris_stream_release_frame(a->stream, &frame));
    EXPECT(iris_stream_release_frame(a->stream, &stale) == IRIS_INVALID_ARGUMENT);
    start_mock(a);
}

static void close_camera(camera_t *camera)
{
    CHECK(iris_stream_stop(camera->stream));
    CHECK(iris_stream_close(&camera->stream));
    CHECK(iris_device_close(&camera->device));
}

static void consume(void *opaque, const iris_frame_t *frame)
{
    camera_t *camera = opaque;
    if (frame->status == IRIS_FRAME_COMPLETE) {
        EXPECT(frame->data && frame->width == camera->width && frame->height == camera->height);
        EXPECT(frame->stride_bytes * frame->height == frame->image_size);
        EXPECT(frame->payload.type == IRIS_PAYLOAD_IMAGE &&
               (frame->payload.valid & IRIS_PAYLOAD_HAS_SIZE_FILLED) &&
               frame->payload.size_filled <= frame->payload.capacity &&
               frame->payload.size_filled >= frame->image_size && frame->payload.data);
        if (camera->buffers) {
            size_t i = 0;
            for (; i < CALLER_BUFFER_COUNT; ++i)
                if (frame->user_data == camera->buffers[i].user_data) break;
            EXPECT(i < CALLER_BUFFER_COUNT);
            EXPECT(frame->payload.data == camera->buffers[i].data &&
                   frame->payload.capacity == camera->buffers[i].size);
        }
        if (frame->valid & IRIS_FRAME_HAS_ID) {
            if (!camera->have_id) camera->first_id = frame->frame_id;
            else if (frame->frame_id > camera->last_id && frame->frame_id - camera->last_id > 1)
                camera->id_gaps += frame->frame_id - camera->last_id - 1;
            camera->last_id = frame->frame_id;
            camera->have_id = 1;
        }
        if (frame->valid & IRIS_FRAME_HAS_HOST_TIMESTAMP_NS) {
            if (camera->last_host && frame->host_timestamp_ns >= camera->last_host) {
                const uint64_t interval = frame->host_timestamp_ns - camera->last_host;
                if (interval > camera->max_interval_ns) camera->max_interval_ns = interval;
            }
            camera->last_host = frame->host_timestamp_ns;
        }
        ++camera->complete;
    } else {
        fprintf(stderr, "frame status %d, PFNC %llx namespace %u flags %u, %llux%llu\n",
                frame->status, (unsigned long long)frame->pixel_format, frame->pixel_format_namespace,
                frame->valid, (unsigned long long)frame->width, (unsigned long long)frame->height);
        ++camera->rejected;
        EXPECT(camera->allow_rejected);
    }
}

static double monotonic(void)
{
    struct timespec t;
    EXPECT(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

static size_t poll_simulator(camera_t *camera)
{
    size_t delivered;
    CHECK(iris_stream_poll(camera->stream, consume, camera, 2, &delivered));
    EXPECT(delivered <= 2 && camera->id_gaps == 0);
    return delivered;
}

static void native_no_frame(camera_t *triggered, camera_t *continuous)
{
    const double deadline = monotonic() + 0.005;
    do {
        iris_frame_t frame = {0};
        EXPECT(iris_stream_try_get_frame(triggered->stream, &frame) == IRIS_NO_FRAME);
        poll_simulator(continuous);
    } while (monotonic() < deadline);
}

static iris_frame_t native_trigger(camera_t *triggered, camera_t *continuous, unsigned marker)
{
    allocation_pause_thread(1);
    CHECK(iris_feature_execute(triggered->remote, "TriggerSoftware"));
    allocation_pause_thread(0);
    const double deadline = monotonic() + 1;
    iris_frame_t frame = {0};
    for (;;) {
        poll_simulator(continuous);
        const iris_status_t status = iris_stream_try_get_frame(triggered->stream, &frame);
        if (status == IRIS_OK) break;
        EXPECT(status == IRIS_NO_FRAME && monotonic() < deadline);
    }
    consume(triggered, &frame);
    EXPECT(frame.image_size > 1 && frame.data == frame.payload.data);
    EXPECT(((const unsigned char *)frame.data)[0] == marker);
    EXPECT(((const unsigned char *)frame.data)[frame.image_size - 1] ==
           (unsigned char)(marker + frame.image_size - 1));
    EXPECT(iris_stream_release_frame(continuous->stream, &frame) == IRIS_INVALID_ARGUMENT);
    return frame;
}

static void expect_locked(iris_node_map_t *map, int64_t expected)
{
    int64_t value;
    CHECK(iris_feature_get_integer(map, "TLParamsLocked", &value));
    EXPECT(value == expected);
}

static void test_native_ring(camera_t *triggered, camera_t *continuous)
{
    enum { depth = 4, cycles = 20 };
    const iris_feature_write_t trigger[] = {
        {"TriggerSelector", "FrameStart"}, {"TriggerMode", "Off"},
        {"TriggerSource", "Software"}, {"TriggerMode", "On"},
    };
    size_t applied;
    CHECK(iris_feature_write_batch(triggered->remote, trigger, sizeof(trigger) / sizeof(*trigger), &applied));
    EXPECT(applied == sizeof(trigger) / sizeof(*trigger));

    iris_buffer_requirements_t requirements;
    CHECK(iris_stream_buffer_requirements(triggered->stream, &requirements));
    EXPECT(requirements.minimum_buffer_count == depth);
    const iris_start_options_t options = {
        (uint32_t)requirements.minimum_buffer_count, IRIS_ACQUISITION_STREAM_ONLY, NULL
    };
    const iris_start_options_t oversized = {depth + 1, IRIS_ACQUISITION_STREAM_ONLY, NULL};
    EXPECT(iris_stream_start(triggered->stream, &oversized) == IRIS_BUSY);
    CHECK(iris_stream_buffer_requirements(triggered->stream, &requirements));
    expect_locked(triggered->remote, 0);

    CHECK(iris_feature_set_float(continuous->remote, "ExposureTime", 2000));
    CHECK(iris_feature_set_enumeration(continuous->remote, "AcquisitionMode", "Continuous"));
    iris_buffer_t continuous_buffers[CALLER_BUFFER_COUNT];
    start_with_buffers(continuous, continuous_buffers, IRIS_ACQUISITION_REMOTE_AND_STREAM);

    iris_frame_t stale = {0};
    for (unsigned cycle = 0; cycle < cycles; ++cycle) {
        CHECK(iris_stream_start(triggered->stream, &options));
        expect_locked(triggered->remote, 1);
        allocation_begin();
        native_no_frame(triggered, continuous);
        iris_frame_t frames[depth];
        for (unsigned i = 0; i < depth; ++i) {
            frames[i] = native_trigger(triggered, continuous, i);
            if (stale.token) EXPECT(iris_stream_release_frame(triggered->stream, &stale) == IRIS_INVALID_ARGUMENT);
            for (unsigned j = 0; j < i; ++j) EXPECT(frames[i].data != frames[j].data);
        }
        const void *oldest = frames[0].data;
        CHECK(iris_stream_release_frame(triggered->stream, &frames[0]));
        frames[0] = native_trigger(triggered, continuous, depth);
        EXPECT(frames[0].data == oldest);
        native_no_frame(triggered, continuous);
        for (unsigned i = 1; i < depth; ++i)
            EXPECT(((const unsigned char *)frames[i].data)[0] == i);
        allocation_counts counts = allocation_end();
        EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);

        CHECK(iris_stream_stop(triggered->stream));
        expect_locked(triggered->remote, 0);
        EXPECT(iris_stream_close(&triggered->stream) == IRIS_BUSY);
        EXPECT(iris_stream_start(triggered->stream, &options) == IRIS_BUSY);
        allocation_begin();
        const double deadline = monotonic() + 1;
        while (!poll_simulator(continuous)) EXPECT(monotonic() < deadline);
        stale = frames[0];
        for (unsigned i = 0; i < depth; ++i) {
            EXPECT(((const unsigned char *)frames[i].data)[0] == (i ? i : depth));
            CHECK(iris_stream_release_frame(triggered->stream, &frames[i]));
            EXPECT(!frames[i].token && !frames[i].data);
        }
        EXPECT(iris_stream_release_frame(triggered->stream, &stale) == IRIS_INVALID_ARGUMENT);
        counts = allocation_end();
        EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
        // Alternate explicit cleanup with restart retiring the released pool.
        if (cycle % 2 == 0) {
            CHECK(iris_stream_stop(triggered->stream));
            expect_locked(triggered->remote, 0);
        }
    }
    CHECK(iris_stream_stop(triggered->stream));
    expect_locked(triggered->remote, 0);
    printf("Native ring: %u cycles, %llu triggered frames, %llu continuous frames, zero Iris allocation calls/bytes\n",
           cycles, (unsigned long long)triggered->complete, (unsigned long long)continuous->complete);
    close_camera(continuous);
    free_buffers(continuous_buffers);
}

static void capture(camera_t *a, camera_t *b, unsigned target)
{
    const uint64_t end_a = a->complete + target;
    const uint64_t end_b = b ? b->complete + target : 0;
    const double started = monotonic();
    const double deadline = started + 15;
    uint64_t triggered = a->complete;
    allocation_begin();
    while (a->complete < end_a || (b && b->complete < end_b)) {
        if (a->software_trigger && triggered == a->complete) {
            allocation_pause_thread(1);
            CHECK(iris_feature_execute(a->remote, "TriggerSoftware"));
            allocation_pause_thread(0);
            ++triggered;
        }
        iris_frame_t frame;
        iris_status_t status = iris_stream_try_get_frame(a->stream, &frame);
        if (status == IRIS_OK) {
            consume(a, &frame);
            if (b) EXPECT(iris_stream_release_frame(b->stream, &frame) == IRIS_INVALID_ARGUMENT);
            CHECK(iris_stream_release_frame(a->stream, &frame));
        } else if (status != IRIS_NO_FRAME) {
            check(status, "try frame");
        }
        if (b) {
            size_t delivered;
            CHECK(iris_stream_poll(b->stream, consume, b, 2, &delivered));
            EXPECT(delivered <= 2);
        }
        if (monotonic() >= deadline) { fprintf(stderr, "capture timeout: %llu, %llu\n", (unsigned long long)a->complete, (unsigned long long)(b ? b->complete : 0)); exit(1); }
        struct timespec pause = {0, 100000};
        nanosleep(&pause, NULL);
    }
    const allocation_counts counts = allocation_end();
    printf("allocations: consumer %llu calls/%llu bytes, other %llu calls/%llu bytes\n",
           (unsigned long long)counts.consumer_calls, (unsigned long long)counts.consumer_bytes,
           (unsigned long long)counts.other_calls, (unsigned long long)counts.other_bytes);
    EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
    printf("complete frames: %llu", (unsigned long long)a->complete);
    if (b) printf(", %llu", (unsigned long long)b->complete);
    printf(" in %.3f s, first stream IDs %llu..%llu, gaps %llu, max dequeue interval %.3f ms\n",
           monotonic() - started, (unsigned long long)a->first_id, (unsigned long long)a->last_id,
           (unsigned long long)a->id_gaps, a->max_interval_ns / 1e6);
}

static void capture_with_xml_override(iris_context_t *context, const char *cti, camera_t *triggered, camera_t *continuous)
{
    static const char feature[] =
        "<Integer Name=\"OverrideWidth\" NameSpace=\"Custom\"><pValue>Width</pValue></Integer>\n";
    size_t size = 0;
    CHECK(iris_node_map_xml(triggered->remote, NULL, &size));
    char *xml = malloc(size);
    EXPECT(xml);
    CHECK(iris_node_map_xml(triggered->remote, xml, &size));
    const char *end = strstr(xml, "</RegisterDescription>");
    EXPECT(end);
    char file[] = "/tmp/iris-triggered #%-XXXXXX";
    const int fd = mkstemp(file);
    EXPECT(fd >= 0);
    FILE *output = fdopen(fd, "wb");
    EXPECT(output);
    const size_t prefix = (size_t)(end - xml);
    EXPECT(fwrite(xml, 1, prefix, output) == prefix);
    EXPECT(fwrite(feature, 1, sizeof(feature) - 1, output) == sizeof(feature) - 1);
    EXPECT(fputs(end, output) >= 0 && fclose(output) == 0);
    free(xml);

    close_camera(triggered);
    open_camera(context, cti, NULL, file, triggered);
    EXPECT(unlink(file) == 0);
    iris_node_map_xml_info_t source;
    CHECK(iris_node_map_xml_info(triggered->remote, &source));
    EXPECT(source.valid == IRIS_XML_IS_OVERRIDE && source.url_index == UINT32_MAX && !source.indexed);
    EXPECT(strncmp(source.url, "file:///tmp/iris-triggered%20%23%25-", sizeof("file:///tmp/iris-triggered%20%23%25-") - 1) == 0);
    char element[512];
    size = sizeof(element);
    CHECK(iris_feature_xml(triggered->remote, "OverrideWidth", element, &size));
    EXPECT(strstr(element, "<pValue>Width</pValue>"));
    iris_integer_range_t width_range;
    CHECK(iris_feature_integer_range(triggered->remote, "Width", &width_range));
    CHECK(iris_feature_set_integer(triggered->remote, "OverrideWidth", width_range.minimum));
    int64_t width;
    CHECK(iris_feature_get_integer(triggered->remote, "Width", &width));
    EXPECT(width == width_range.minimum);
    CHECK(iris_feature_set_integer(triggered->remote, "OverrideWidth", (int64_t)triggered->width));
    CHECK(iris_feature_get_integer(triggered->remote, "OverrideWidth", &width));
    EXPECT(width == (int64_t)triggered->width);

    CHECK(iris_node_map_xml_info(continuous->remote, &source));
    EXPECT(source.url && *source.url && !(source.valid & IRIS_XML_IS_OVERRIDE));
    EXPECT(iris_feature_get_integer(continuous->remote, "OverrideWidth", &width) == IRIS_NOT_FOUND);
    CHECK(iris_feature_set_float(continuous->remote, "ExposureTime", 2000));
    CHECK(iris_feature_set_enumeration(continuous->remote, "AcquisitionMode", "Continuous"));
    iris_buffer_t triggered_buffers[CALLER_BUFFER_COUNT], continuous_buffers[CALLER_BUFFER_COUNT];
    start_with_buffers(triggered, triggered_buffers, IRIS_ACQUISITION_STREAM_ONLY);
    start_with_buffers(continuous, continuous_buffers, IRIS_ACQUISITION_REMOTE_AND_STREAM);
    capture(triggered, continuous, 50);
    CHECK(iris_stream_stop(triggered->stream));
    close_camera(continuous);
    free_buffers(triggered_buffers);
    free_buffers(continuous_buffers);
}

int main(int argc, char **argv)
{
    EXPECT(argc >= 3);
    allocation_begin();
    void *(*volatile allocate)(size_t) = malloc;
    void *probe = allocate(13);
    const allocation_counts calibration = allocation_end();
    EXPECT(probe && calibration.other_calls == 1 && calibration.other_bytes == 13);
    free(probe);
    iris_context_t *context = NULL;
    EXPECT(iris_abi_major() == 1);
    allocation_begin();
    CHECK(iris_context_create(&context));
    const allocation_counts context_allocations = allocation_end();
    EXPECT(context_allocations.consumer_calls > 0 && context_allocations.consumer_bytes > 0);
    if (strcmp(argv[1], "--list") == 0) {
        iris_device_list_t *list = NULL;
        CHECK(iris_discover(context, argv[2], 1000, &list));
        for (size_t i = 0; i < iris_device_list_count(list); ++i) {
            iris_device_info_t info;
            CHECK(iris_device_list_get(list, i, &info));
            printf("%s %s %s %s\n", info.interface_id, info.device_id,
                   info.serial_number ? info.serial_number : "-", info.model ? info.model : "-");
        }
        iris_device_list_close(&list);
    } else {
        camera_t a = {0}, b = {0};
        const int native_ring = strcmp(argv[1], "--test-native-ring") == 0;
        const int xml_override = strcmp(argv[1], "--test-xml-override") == 0;
        const int caller_buffers = strcmp(argv[1], "--test-caller-buffers") == 0;
        const int simulated = caller_buffers || strcmp(argv[1], "--test") == 0;
        const int cycling = strcmp(argv[1], "--restart-bench") == 0;
        const int bench_mixed = cycling || strcmp(argv[1], "--mixed-bench") == 0;
        const int mixed = native_ring || xml_override || simulated || bench_mixed;
        EXPECT(mixed || strcmp(argv[1], "--capture") == 0);
        EXPECT(!mixed || argc == (cycling ? 5 : 4));
        open_camera(context, argv[2], !mixed && argc == 4 ? argv[3] : NULL, NULL, &a);
        if (native_ring) {
            open_camera(context, argv[3], "test-serial", NULL, &b);
            test_native_ring(&a, &b);
        } else if (xml_override) {
            open_camera(context, argv[3], "test-serial", NULL, &b);
            capture_with_xml_override(context, argv[2], &a, &b);
        } else if (simulated) {
            iris_buffer_t a_buffers[CALLER_BUFFER_COUNT], b_buffers[CALLER_BUFFER_COUNT];
            open_camera(context, argv[3], "test-serial", NULL, &b);
            test_features(&a, &b);
            if (!caller_buffers) {
                start_mock(&a);
                test_wait(&a);
            }
            CHECK(iris_node_map_enable_invalidation(b.remote, 1));
            CHECK(iris_feature_set_float(b.remote, "ExposureTime", 2000));
            size_t invalidations;
            CHECK(iris_node_map_poll_invalidation(b.remote, 8, &invalidations));
            EXPECT(invalidations >= 1);
            CHECK(iris_node_map_enable_invalidation(b.remote, 0));
            double exposure;
            CHECK(iris_feature_get_float(b.remote, "ExposureTime", &exposure));
            EXPECT(exposure > 1999.0 && exposure < 2001.0);
            CHECK(iris_feature_set_enumeration(b.remote, "AcquisitionMode", "Continuous"));
            char mode[32];
            size_t mode_size = sizeof(mode);
            CHECK(iris_feature_get_enumeration(b.remote, "AcquisitionMode", mode, &mode_size));
            EXPECT(strcmp(mode, "Continuous") == 0);
            if (caller_buffers) {
                test_simulator_write_stacks(&a, &b);
                start_with_buffers(&a, a_buffers, IRIS_ACQUISITION_STREAM_ONLY);
                start_with_buffers(&b, b_buffers, IRIS_ACQUISITION_REMOTE_AND_STREAM);
            } else {
                CHECK(iris_stream_start(b.stream, NULL));
            }
            capture(&a, &b, 50);
            close_camera(&a);
            if (caller_buffers) free_buffers(a_buffers);
            capture(&b, NULL, 5);
            open_camera(context, argv[2], NULL, NULL, &a);
            start_mock(&a);
            close_camera(&b);
            if (caller_buffers) free_buffers(b_buffers);
            capture(&a, NULL, 5);
        } else if (bench_mixed) {
            open_camera(context, argv[3], NULL, NULL, &b);
            CHECK(iris_stream_start(a.stream, NULL));
            start_mock(&b);
            if (cycling) {
                char *end = NULL;
                const unsigned long cycles = strtoul(argv[4], &end, 10);
                EXPECT(*argv[4] && !*end && cycles > 0);
                a.allow_rejected = 1;
                for (unsigned long cycle = 1; cycle <= cycles; ++cycle) {
                    fprintf(stderr, "cycle %lu\n", cycle);
                    capture(&b, &a, 5);
                    CHECK(iris_stream_stop(a.stream));
                    a.last_host = 0;
                    if (cycle < cycles) CHECK(iris_stream_start(a.stream, NULL));
                }
                printf("rejected capture producer completions: %u\n", a.rejected);
                close_camera(&b);
                close_camera(&a);
                CHECK(iris_context_close(&context));
                return a.rejected ? 1 : 0;
            }
            capture(&b, &a, 100);
            close_camera(&b);
            capture(&a, NULL, 25);
            CHECK(iris_stream_stop(a.stream));
            a.last_host = 0;
            CHECK(iris_stream_start(a.stream, NULL));
            capture(&a, NULL, 25);
        } else {
            CHECK(iris_stream_start(a.stream, NULL));
            capture(&a, NULL, 100);
        }
        close_camera(&a);
    }
    CHECK(iris_context_close(&context));
    return 0;
}
