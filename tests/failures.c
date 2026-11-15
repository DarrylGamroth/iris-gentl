#define _POSIX_C_SOURCE 200809L
#include "iris.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) EXPECT((x) == IRIS_OK)

struct wait_result { iris_stream_t *stream; iris_status_t status; };
static void *waiter(void *data)
{
    struct wait_result *wait = data;
    iris_frame_t frame;
    wait->status = iris_stream_wait_frame(wait->stream, 1000, &frame);
    return NULL;
}
static void ignore_frame(void *data, const iris_frame_t *frame) { (void)data; (void)frame; }

static void run(const char *path, const char *scenario)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_stream_t *stream = NULL;
    iris_device_list_t *list = NULL;
    iris_device_selector_t selector = {path, NULL, NULL, "test-serial"};
    OK(iris_context_create(&context));
    if (!strcmp(scenario, "interface") || !strcmp(scenario, "discover")) {
        EXPECT(iris_discover(context, path, 0, &list) < 0 && !list);
    } else if (!strcmp(scenario, "device")) {
        iris_status_t result = iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device);
        EXPECT(result == IRIS_BUSY);
    } else if (!strcmp(scenario, "ambiguous")) {
        EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_AMBIGUOUS);
    } else if (!strcmp(scenario, "throw-device") || !strcmp(scenario, "throw-interface")) {
        selector.interface_id = "test-interface";
        selector.device_id = "test-device";
        selector.serial_number = NULL;
        if (!strcmp(scenario, "throw-interface")) {
            EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_IO_ERROR);
            EXPECT(device == NULL);
            EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
        } else {
            OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
            iris_device_t *peer = NULL;
            selector.device_id = "test-device-2";
            EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &peer) == IRIS_IO_ERROR);
            EXPECT(peer == NULL);
            OK(iris_device_close(&device));
            EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
        }
    } else if (!strcmp(scenario, "discover-close")) {
        EXPECT(iris_discover(context, path, 0, &list) == IRIS_BUSY && !list);
        EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
    } else if (!strcmp(scenario, "missing-device")) {
        EXPECT(setenv("IRIS_FAULT", "close-system", 1) == 0);
        selector.serial_number = "absent";
        EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_NOT_FOUND);
        EXPECT(!device);
        iris_error_t error;
        iris_last_error(&error);
        EXPECT(!strcmp(error.operation, "device open") && !error.native_code_valid);
        void *resident = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        EXPECT(resident && dlclose(resident) == 0);
        EXPECT(unsetenv("IRIS_FAULT") == 0);
        OK(iris_discover(context, path, 0, &list));
        iris_device_list_close(&list);
        EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
    } else if (!strncmp(scenario, "partial-stream-", 15)) {
        OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
        const iris_status_t status = iris_stream_open(device, 0, &stream);
        EXPECT(status == (!strcmp(scenario, "partial-stream-throw") ? IRIS_IO_ERROR : IRIS_PRODUCER_ERROR));
        EXPECT(!stream);
        iris_error_t error;
        iris_last_error(&error);
        EXPECT(!strcmp(error.operation, "DevOpenDataStream"));
        OK(iris_device_close(&device));
        EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
    } else if (!strncmp(scenario, "close-", 6)) {
        OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
        EXPECT(iris_device_close(&device) == IRIS_BUSY);
        EXPECT(device != NULL);
        size_t count;
        iris_node_map_t *map = NULL;
        EXPECT(iris_device_stream_count(device, &count) == IRIS_INVALID_STATE);
        EXPECT(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map) == IRIS_INVALID_STATE);
        EXPECT(iris_stream_open(device, 0, &stream) == IRIS_INVALID_STATE);
        void *resident = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
        EXPECT(resident != NULL);
        EXPECT(dlclose(resident) == 0);
        OK(iris_device_close(&device));
        EXPECT(device == NULL);
        EXPECT(dlopen(path, RTLD_NOW | RTLD_NOLOAD) == NULL);
    } else {
        OK(iris_device_open(context, &selector, IRIS_DEVICE_EXCLUSIVE, 0, &device));
        EXPECT(iris_context_close(&context) == IRIS_BUSY);
        OK(iris_stream_open(device, 0, &stream));
        EXPECT(iris_device_close(&device) == IRIS_BUSY);
        iris_node_map_t *map;
        EXPECT(iris_stream_node_map(stream, &map) == IRIS_NOT_SUPPORTED);
        _Alignas(16) unsigned char memory[32] = {42};
        iris_buffer_t buffers[] = {{memory, 16, memory}, {memory + 16, 16, NULL}};
        iris_start_options_t options = {1, IRIS_ACQUISITION_STREAM_ONLY, NULL};
        if (!strncmp(scenario, "caller-", 7)) {
            iris_buffer_requirements_t requirements;
            OK(iris_stream_buffer_requirements(stream, &requirements));
            EXPECT(requirements.payload_size == 16 && requirements.alignment == 16 &&
                   requirements.minimum_buffer_count == 1);
            options.buffers = buffers;
            options.buffer_count = 0;
            EXPECT(iris_stream_start(stream, &options) == IRIS_INVALID_ARGUMENT);
            options.buffer_count = 1;
            buffers[0].size = 15;
            EXPECT(iris_stream_start(stream, &options) == IRIS_BUFFER_TOO_SMALL);
            buffers[0].size = 16;
            buffers[0].data = memory + 1;
            EXPECT(iris_stream_start(stream, &options) == IRIS_INVALID_ARGUMENT);
            buffers[0].data = memory;
            buffers[0].size = SIZE_MAX;
            EXPECT(iris_stream_start(stream, &options) == IRIS_INVALID_ARGUMENT);
            options.buffer_count = 2;
            buffers[0].size = sizeof(memory);
            EXPECT(iris_stream_start(stream, &options) == IRIS_INVALID_ARGUMENT);
            buffers[0].size = 16;
            options.buffer_count = 1;
        }
        const iris_status_t started = iris_stream_start(stream, &options);
        if (!strcmp(scenario, "caller-rollback")) {
            EXPECT(started == IRIS_PRODUCER_ERROR);
            iris_error_t error;
            iris_last_error(&error);
            EXPECT(!strcmp(error.operation, "DSAnnounceBuffer"));
            EXPECT(iris_stream_buffer_requirements(stream, &(iris_buffer_requirements_t){0}) == IRIS_BUSY);
            OK(iris_stream_stop(stream));
            EXPECT(memory[0] == 42);
        } else if (!strcmp(scenario, "caller-unsupported")) {
            EXPECT(started == IRIS_NOT_SUPPORTED);
            OK(iris_stream_stop(stream));
            EXPECT(memory[0] == 42);
        } else if (!strcmp(scenario, "event-null")) {
            EXPECT(started == IRIS_PRODUCER_ERROR);
            iris_error_t error;
            iris_last_error(&error);
            EXPECT(!strcmp(error.operation, "GCRegisterEvent"));
            EXPECT(iris_stream_close(&stream) == IRIS_BUSY);
            OK(iris_stream_stop(stream));
            OK(iris_stream_start(stream, &options));
            OK(iris_stream_stop(stream));
        } else if (!strncmp(scenario, "armed-", 6)) {
            EXPECT(started == (!strcmp(scenario, "armed-throw") ? IRIS_IO_ERROR : IRIS_PRODUCER_ERROR));
            iris_error_t error;
            iris_last_error(&error);
            EXPECT(!strcmp(error.operation, "DSStartAcquisition"));
            EXPECT(iris_stream_close(&stream) == IRIS_BUSY);
            OK(iris_stream_stop(stream));
        } else if (!strcmp(scenario, "start") || !strcmp(scenario, "queue") || !strcmp(scenario, "allocate")) {
            EXPECT(started < 0);
        } else {
            OK(started);
            iris_frame_t frame;
            if (!strcmp(scenario, "timeout")) {
                EXPECT(iris_stream_wait_frame(stream, 23, &frame) == IRIS_TIMEOUT);
                EXPECT(iris_stream_try_get_frame(stream, &frame) == IRIS_NO_FRAME);
                struct wait_result wait = {stream, IRIS_OK};
                pthread_t thread;
                EXPECT(pthread_create(&thread, NULL, waiter, &wait) == 0);
                const struct timespec pause = {0, 5000000};
                nanosleep(&pause, NULL);
                EXPECT(iris_stream_try_get_frame(stream, &frame) == IRIS_BUSY);
                OK(iris_stream_stop(stream));
                EXPECT(pthread_join(thread, NULL) == 0 && wait.status == IRIS_STOPPED);
            } else if (!strcmp(scenario, "event") || !strcmp(scenario, "abort") || !strcmp(scenario, "throw-event") || !strcmp(scenario, "io-event")) {
                size_t delivered;
                const iris_status_t status = iris_stream_poll(stream, ignore_frame, NULL, 1, &delivered);
                EXPECT(status < 0);
                if (!strcmp(scenario, "io-event")) {
                    EXPECT(status == IRIS_PRODUCER_ERROR);
                    iris_error_t error;
                    iris_last_error(&error);
                    EXPECT(error.native_code_valid && error.native_code == -1010);
                }
                EXPECT(delivered == 0);
                if (!strcmp(scenario, "throw-event")) {
                    iris_error_t error;
                    iris_last_error(&error);
                    EXPECT(!strcmp(error.operation, "EventGetData"));
                    EXPECT(!strcmp(error.message, "injected event exception"));
                }
            } else {
                OK(iris_stream_try_get_frame(stream, &frame));
                iris_frame_status_t expected = IRIS_FRAME_COMPLETE;
                if (!strcmp(scenario, "invalid") || !strcmp(scenario, "height-invalid") ||
                    !strcmp(scenario, "caller-wrong-base")) expected = IRIS_FRAME_INVALID_IMAGE;
                if (!strcmp(scenario, "payload") || !strcmp(scenario, "packed-odd")) expected = IRIS_FRAME_UNSUPPORTED_PAYLOAD;
                if (!strcmp(scenario, "incomplete") || !strcmp(scenario, "short-image") || !strcmp(scenario, "height-short")) expected = IRIS_FRAME_INCOMPLETE;
                EXPECT(frame.status == expected);
                if (!strcmp(scenario, "packed")) EXPECT(frame.stride_bytes == 7 && frame.image_size == 14);
                if (!strcmp(scenario, "payload")) {
                    EXPECT(frame.payload.type == IRIS_PAYLOAD_RAW_DATA && frame.payload.data &&
                           frame.payload.size_filled == 16 && frame.payload.data_size == 16 &&
                           (frame.payload.valid & IRIS_PAYLOAD_HAS_CHUNK_LAYOUT_ID) &&
                           frame.payload.contains_chunk_data && frame.payload.chunk_layout_id == 123);
                }
                if (!strcmp(scenario, "invalid") || !strcmp(scenario, "caller-wrong-base"))
                    EXPECT(!frame.payload.data);
                if (!strcmp(scenario, "incomplete"))
                    EXPECT(frame.payload.incomplete && frame.payload.data);
                if (options.buffers) {
                    EXPECT(frame.user_data == memory && frame.payload.capacity == 16);
                    if (expected == IRIS_FRAME_COMPLETE) EXPECT(frame.payload.data == memory);
                    EXPECT(iris_stream_buffer_requirements(stream, &(iris_buffer_requirements_t){0}) == IRIS_BUSY);
                }
                if (expected != IRIS_FRAME_COMPLETE) EXPECT(!frame.data && !frame.image_size);
                const iris_status_t released = iris_stream_release_frame(stream, &frame);
                const iris_status_t expected_release = !strcmp(scenario, "throw-requeue") ? IRIS_IO_ERROR :
                    !strcmp(scenario, "requeue") ? IRIS_PRODUCER_ERROR : IRIS_OK;
                EXPECT(released == expected_release);
                EXPECT(!frame.token);
            }
            const iris_status_t stopped = iris_stream_stop(stream);
            if (!strcmp(scenario, "revoke") || !strcmp(scenario, "stop") || !strcmp(scenario, "throw-stop")) {
                EXPECT(stopped == (!strcmp(scenario, "throw-stop") ? IRIS_IO_ERROR : IRIS_BUSY));
                OK(iris_stream_stop(stream));
            } else {
                OK(stopped);
            }
        }
        OK(iris_stream_close(&stream));
        OK(iris_device_close(&device));
    }
    OK(iris_context_close(&context));
}

static void test_producer_alias(const char *path)
{
    const size_t capacity = strlen(path) + sizeof(".links-XXXXXX/alias.cti");
    char *directory = malloc(capacity);
    char *alias = malloc(capacity);
    EXPECT(directory && alias);
    snprintf(directory, capacity, "%s.links-XXXXXX", path);
    EXPECT(mkdtemp(directory));
    snprintf(alias, capacity, "%s/alias.cti", directory);
    EXPECT(link(path, alias) == 0);

    EXPECT(setenv("IRIS_FAULT", "close-system", 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_device_list_t *list = NULL;
    const iris_device_selector_t selector = {path, "test-interface", "test-device", NULL};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_discover(context, alias, 0, &list));
    iris_device_list_close(&list);
    EXPECT(iris_device_close(&device) == IRIS_BUSY);
    EXPECT(iris_discover(context, alias, 0, &list) == IRIS_BUSY && !list);
    OK(iris_device_close(&device));

    EXPECT(unsetenv("IRIS_FAULT") == 0);
    OK(iris_discover(context, alias, 0, &list));
    iris_device_list_close(&list);
    OK(iris_context_close(&context));
    EXPECT(unlink(alias) == 0 && rmdir(directory) == 0);
    free(alias);
    free(directory);
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    iris_context_t *context = NULL;
    iris_device_list_t *list = NULL;
    OK(iris_context_create(&context));
    EXPECT(iris_discover(context, "/does/not/exist.cti", 0, &list) == IRIS_NOT_FOUND);
    EXPECT(iris_discover(context, "/dev/null", 0, &list) == IRIS_IO_ERROR);
    EXPECT(iris_discover(context, "./bad.cti", 0, &list) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_discover(context, "does-not-exist.cti", 0, &list) == IRIS_NOT_FOUND);
    OK(iris_context_close(&context));
    const char *cases[] = {"interface", "discover", "device", "ambiguous", "allocate",
        "caller-success", "caller-wrong-base", "caller-rollback", "caller-unsupported",
        "throw-interface", "throw-device", "close-interface", "close-system", "close-library",
        "discover-close", "missing-device", "partial-stream-error", "partial-stream-throw", "armed-error", "armed-throw",
        "announce", "queue", "start", "revoke", "stop", "timeout", "abort", "event", "event-null",
        "incomplete", "short-image", "height-short", "height-invalid", "packed", "packed-odd", "payload", "invalid", "requeue", "io-event", "throw-event", "throw-requeue", "throw-stop"};
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) run(argv[1], cases[i]);
    test_producer_alias(argv[1]);
    return 0;
}
