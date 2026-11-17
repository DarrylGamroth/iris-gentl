#include <iris.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return 1;
    iris_error_t error = {0};
    if (status < 0) iris_last_error(&error);
    fprintf(stderr, "%s returned %" PRId32 ": %s\n", call, status,
            error.message ? error.message : "");
    return 0;
}

#define CHECK(call) check((call), #call)

static int check_trigger_enum(iris_node_map_t *remote, const char *name, const char *symbol)
{
    char actual[32];
    size_t size = sizeof(actual);
    if (!check(iris_feature_get_enumeration(remote, name, actual, &size), name))
        return 0;
    if (strcmp(actual, symbol) != 0) {
        fprintf(stderr, "%s read back as %s, expected %s\n", name, actual, symbol);
        return 0;
    }
    printf("%s = %s\n", name, actual);
    return 1;
}

static int configure_trigger(iris_node_map_t *remote)
{
    // Select the trigger first. Turn it off while changing its source.
    const iris_feature_write_t writes[] = {
        {"TriggerSelector", "FrameStart"},
        {"TriggerMode", "Off"},
        {"TriggerSource", "Software"},
        {"TriggerMode", "On"},
    };
    size_t applied;
    if (!check(iris_feature_write_batch(remote, writes, sizeof writes / sizeof *writes, &applied),
               "configure trigger")) {
        fprintf(stderr, "Trigger configuration stopped after %zu writes\n", applied);
        return 0;
    }
    if (!check_trigger_enum(remote, "TriggerSelector", "FrameStart") ||
        !check_trigger_enum(remote, "TriggerSource", "Software") ||
        !check_trigger_enum(remote, "TriggerMode", "On"))
        return 0;

    iris_feature_info_t command;
    if (!CHECK(iris_feature_info(remote, "TriggerSoftware", &command))) return 0;
    if (command.type != IRIS_FEATURE_COMMAND ||
        (command.access != IRIS_FEATURE_READ_WRITE && command.access != IRIS_FEATURE_WRITE_ONLY)) {
        fputs("Expected a writable TriggerSoftware command\n", stderr);
        return 0;
    }
    return 1;
}

static int acquire_one(iris_stream_t *stream, iris_node_map_t *remote)
{
    if (!CHECK(iris_feature_execute(remote, "TriggerSoftware"))) return 0;
    puts("Issued TriggerSoftware");

    iris_frame_t frame = {0};
    if (!CHECK(iris_stream_wait_frame(stream, 1000, &frame))) return 0;
    const int complete = frame.status == IRIS_FRAME_COMPLETE && frame.data && frame.image_size > 0;
    if (complete) {
        // Use frame.data here, while the image is borrowed from the stream.
        printf("Complete frame: %" PRIu64 " x %" PRIu64 ", %zu bytes\n",
               frame.width, frame.height, frame.image_size);
    } else {
        fprintf(stderr, "Expected a complete image, got frame status %" PRId32 "\n", frame.status);
    }
    // IRIS_OK from wait transfers a borrow even when the image is incomplete.
    return CHECK(iris_stream_release_frame(stream, &frame)) && complete;
}

static int expect_no_frame(iris_stream_t *stream)
{
    iris_frame_t frame = {0};
    const iris_status_t status = iris_stream_wait_frame(stream, 25, &frame);
    if (status == IRIS_TIMEOUT) return 1;
    if (status == IRIS_OK) {
        fputs("Received an unexpected completion without a software trigger\n", stderr);
        CHECK(iris_stream_release_frame(stream, &frame));
        return 0;
    }
    return check(status, "wait for an empty stream");
}

int main(int argc, char **argv)
{
    const char *mock = getenv("IRIS_TEST_SOFTWARE_TRIGGER");
    const char *fixture = getenv("IRIS_FAULT");
    const int in_tree = fixture && strcmp(fixture, "progressive-trigger") == 0;
    if (argc != 2 || (!in_tree && (!mock || strcmp(mock, "1") != 0))) {
        fprintf(stderr, "Usage: set IRIS_FAULT=progressive-trigger or IRIS_TEST_SOFTWARE_TRIGGER=1, then run %s MOCK_CTI\n", argv[0]);
        return EXIT_FAILURE;
    }

    iris_context_t *context = NULL;
    iris_device_list_t *devices = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *remote = NULL;
    iris_stream_t *stream = NULL;
    iris_device_info_t camera;
    // The in-tree fixture supports one announced buffer. Both mocks accept stream-only startup.
    const iris_start_options_t options = {in_tree ? 1u : 0u, IRIS_ACQUISITION_STREAM_ONLY, NULL};
    int ok = CHECK(iris_context_create(&context)) &&
             CHECK(iris_discover(context, argv[1], 1000, &devices));
    if (ok && iris_device_list_count(devices) != 1) {
        fputs("Expected exactly one mock device\n", stderr);
        ok = 0;
    }
    if (ok && CHECK(iris_device_list_get(devices, 0, &camera))) {
        const iris_device_selector_t selector = {
            camera.producer_path, camera.interface_id, camera.device_id, NULL
        };
        ok = CHECK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 1000, &device));
    } else {
        ok = 0;
    }
    iris_device_list_close(&devices);

    ok = ok && CHECK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote)) &&
             configure_trigger(remote) &&
             CHECK(iris_stream_open(device, 0, &stream)) &&
             CHECK(iris_stream_start(stream, &options)) &&
             expect_no_frame(stream) &&
             acquire_one(stream, remote) &&
             expect_no_frame(stream);

    // Stop also attempts cleanup after a partially successful start.
    if (stream) {
        if (!CHECK(iris_stream_stop(stream))) ok = 0;
        if (!CHECK(iris_stream_close(&stream))) ok = 0;
    }
    if (device && !CHECK(iris_device_close(&device))) ok = 0;
    if (context && !CHECK(iris_context_close(&context))) ok = 0;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
