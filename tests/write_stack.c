#include "iris.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(x) do { if (!(x)) { \
    iris_error_t error; iris_last_error(&error); \
    fprintf(stderr, "%d: %s (%s: %s)\n", __LINE__, #x, error.operation, error.message); \
    exit(1); \
} } while (0)

static void expect_enum(iris_node_map_t *map, const char *name, const char *expected)
{
    char value[32];
    size_t size = sizeof(value);
    EXPECT(iris_feature_get_enumeration(map, name, value, &size) == IRIS_OK);
    EXPECT(strcmp(value, expected) == 0);
}

static void stage_trigger_modes(iris_node_map_t *map, int invalid_width)
{
    char selector[] = "TriggerSelector";
    char exposure[] = "ExposureStart";
    EXPECT(iris_node_map_stack_add(map, selector, exposure) == IRIS_OK);
    selector[0] = exposure[0] = 'x';
    EXPECT(iris_node_map_stack_add(map, "TriggerMode", "On") == IRIS_OK);
    if (invalid_width) EXPECT(iris_node_map_stack_add(map, "Width", "80") == IRIS_OK);
    EXPECT(iris_node_map_stack_add(map, "TriggerSelector", "FrameStart") == IRIS_OK);
    EXPECT(iris_node_map_stack_add(map, "TriggerMode", "On") == IRIS_OK);
}

static void test_stack(const char *cti, const char *scenario, iris_status_t expected,
                       size_t expected_writes, size_t expected_calls)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *map = NULL;
    const iris_device_selector_t selector = {cti, "test-interface", "test-device", NULL};
    EXPECT(iris_context_create(&context) == IRIS_OK);
    EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_OK);
    EXPECT(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map) == IRIS_OK);

    void *library = dlopen(cti, RTLD_NOW | RTLD_NOLOAD);
    EXPECT(library);
    void (*counts)(size_t *, size_t *) = dlsym(library, "iris_test_stack_counts");
    EXPECT(counts);
    size_t native_calls, scalar_calls, written;
    EXPECT(iris_node_map_write_stack(map, &written) == IRIS_INVALID_STATE && written == 0);
    EXPECT(iris_node_map_stack_add(map, "TriggerMode", "On") == IRIS_INVALID_STATE);
    EXPECT(iris_node_map_start_stacking(map) == IRIS_OK);
    EXPECT(iris_node_map_start_stacking(map) == IRIS_BUSY);
    EXPECT(iris_node_map_stack_add(map, "MissingNode", "On") == IRIS_NOT_FOUND);
    EXPECT(iris_node_map_stack_add(map, "TriggerSoftware", "Execute") == IRIS_WRONG_TYPE);
    stage_trigger_modes(map, 0);
    expect_enum(map, "TriggerSelector", "FrameStart");
    expect_enum(map, "TriggerMode", "Off");
    EXPECT(iris_node_map_cancel_stack(map) == IRIS_OK);
    EXPECT(iris_node_map_cancel_stack(map) == IRIS_OK);
    EXPECT(iris_node_map_start_stacking(map) == IRIS_OK);
    EXPECT(iris_node_map_write_stack(map, &written) == IRIS_OK && written == 0);
    counts(&native_calls, &scalar_calls);
    EXPECT(native_calls == 0 && scalar_calls == 0);

    EXPECT(iris_node_map_start_stacking(map) == IRIS_OK);
    size_t expected_scalar_calls = 0;
    if (expected == IRIS_OK) {
        EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "ExposureStart") == IRIS_OK);
        expect_enum(map, "TriggerSelector", "ExposureStart");
        EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "FrameStart") == IRIS_OK);
        expected_scalar_calls = 2;
    }
    stage_trigger_modes(map, expected == IRIS_GENAPI_ERROR);
    EXPECT(iris_node_map_write_stack(map, NULL) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_node_map_write_stack(map, &written) == expected && written == expected_writes);
    iris_error_t error;
    iris_last_error(&error);
    if (expected == IRIS_GENAPI_ERROR) EXPECT(strstr(error.message, "Width"));
    if (strcmp(scenario, "stacking-partial") == 0)
        EXPECT(error.native_code_valid && strcmp(error.operation, "GCWritePortStacked") == 0);
    counts(&native_calls, &scalar_calls);
    EXPECT(native_calls == expected_calls && scalar_calls == expected_scalar_calls);
    EXPECT(iris_node_map_write_stack(map, &written) == IRIS_INVALID_STATE && written == 0);

    expect_enum(map, "TriggerSelector", expected_writes == 2 ? "ExposureStart" : "FrameStart");
    EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "ExposureStart") == IRIS_OK);
    expect_enum(map, "TriggerMode", expected_writes ? "On" : "Off");
    EXPECT(iris_feature_set_enumeration(map, "TriggerSelector", "FrameStart") == IRIS_OK);
    expect_enum(map, "TriggerMode", expected_writes == 4 ? "On" : "Off");

    EXPECT(iris_node_map_start_stacking(map) == IRIS_OK);
    EXPECT(iris_node_map_stack_add(map, "TriggerMode", "Off") == IRIS_OK);
    EXPECT(dlclose(library) == 0);
    EXPECT(iris_device_close(&device) == IRIS_OK);
    EXPECT(iris_context_close(&context) == IRIS_OK);
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    test_stack(argv[1], "stacking", IRIS_OK, 4, 1);
    test_stack(argv[1], "stacking", IRIS_GENAPI_ERROR, 4, 2);
    test_stack(argv[1], "stacking-partial", IRIS_PRODUCER_ERROR, 2, 1);
    test_stack(argv[1], "stacking-short", IRIS_PRODUCER_ERROR, 2, 1);
    test_stack(argv[1], "stacking-unsupported", IRIS_NOT_SUPPORTED, 0, 1);
    test_stack(argv[1], "stacking-throw", IRIS_IO_ERROR, 0, 1);
}
