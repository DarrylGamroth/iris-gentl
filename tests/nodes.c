#define _POSIX_C_SOURCE 200809L
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void expect_enum(iris_node_map_t *map, const char *name, const char *expected)
{
    char value[32];
    size_t size = sizeof(value);
    OK(iris_feature_get_enumeration(map, name, value, &size));
    EXPECT(strcmp(value, expected) == 0);
}

static void select_trigger(iris_node_map_t *map, const char *symbol)
{
    OK(iris_feature_set_enumeration(map, "TriggerSelector", symbol));
    expect_enum(map, "TriggerSelector", symbol);
}

static void test_selector(iris_node_map_t *map)
{
    select_trigger(map, "FrameStart");
    expect_enum(map, "TriggerMode", "Off");
    OK(iris_feature_set_enumeration(map, "TriggerMode", "On"));
    expect_enum(map, "TriggerMode", "On");

    select_trigger(map, "ExposureStart");
    expect_enum(map, "TriggerMode", "Off");
    OK(iris_feature_from_string(map, "TriggerMode", "On"));
    expect_enum(map, "TriggerMode", "On");

    select_trigger(map, "FrameStart");
    expect_enum(map, "TriggerMode", "On");
    OK(iris_feature_set_enumeration(map, "TriggerMode", "Off"));
    expect_enum(map, "TriggerMode", "Off");
    select_trigger(map, "ExposureStart");
    expect_enum(map, "TriggerMode", "On");
    select_trigger(map, "FrameStart");
    expect_enum(map, "TriggerMode", "Off");
}

static void test_registers(iris_node_map_t *map)
{
    const char *name = "TriggerModeReg";
    const unsigned char on[4] = {1, 0, 0, 0}, off[4] = {0};
    unsigned char bytes[8];
    iris_register_info_t info;
    select_trigger(map, "FrameStart");
    OK(iris_feature_register_info(map, name, &info));
    EXPECT(info.address == 4 && info.length == sizeof on);
    size_t size = 0;
    OK(iris_feature_read_register(map, name, NULL, &size));
    EXPECT(size == sizeof on);
    size = 2;
    memset(bytes, 0xa5, sizeof bytes);
    EXPECT(iris_feature_read_register(map, name, bytes, &size) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(size == sizeof on && bytes[0] == 0xa5 && bytes[1] == 0xa5);
    EXPECT(iris_feature_write_register(map, name, on, 1) == IRIS_OUT_OF_RANGE);
    OK(iris_feature_write_register(map, name, on, sizeof on));
    expect_enum(map, "TriggerMode", "On");

    select_trigger(map, "ExposureStart");
    OK(iris_feature_register_info(map, name, &info));
    EXPECT(info.address == 8 && info.length == sizeof off);
    OK(iris_feature_write_register(map, name, off, sizeof off));
    expect_enum(map, "TriggerMode", "Off");
    size = sizeof bytes;
    OK(iris_feature_read_register(map, name, bytes, &size));
    EXPECT(size == sizeof off && memcmp(bytes, off, size) == 0 && bytes[size] == 0xa5);

    select_trigger(map, "FrameStart");
    OK(iris_feature_read_register(map, name, bytes, &size));
    EXPECT(memcmp(bytes, on, sizeof on) == 0);
    EXPECT(iris_feature_register_info(map, "Width", &info) == IRIS_WRONG_TYPE);
    EXPECT(iris_feature_write_register(map, "WidthMaxReg", on, sizeof on) == IRIS_ACCESS_DENIED);
    EXPECT(iris_feature_read_register(map, "ChangeWidthMaxReg", bytes, &size) == IRIS_ACCESS_DENIED);
    iris_feature_info_t feature;
    OK(iris_feature_info(map, "RawData", &feature));
    EXPECT(feature.type == IRIS_FEATURE_REGISTER && feature.access == IRIS_FEATURE_READ_WRITE);
    const unsigned char binary[] = {0x00, 0xff, 0x80, 0x12};
    OK(iris_feature_write_register(map, "RawData", binary, sizeof binary));
    OK(iris_feature_read_register(map, "RawData", bytes, &size));
    EXPECT(size == sizeof binary && memcmp(bytes, binary, size) == 0);
}

static void test_boolean(iris_node_map_t *map)
{
    iris_feature_info_t info;
    OK(iris_feature_info(map, "ReverseX", &info));
    EXPECT(info.type == IRIS_FEATURE_BOOLEAN && info.access == IRIS_FEATURE_READ_WRITE);
    const uint32_t values[] = {1, 0};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        uint32_t value;
        OK(iris_feature_set_boolean(map, "ReverseX", values[i]));
        OK(iris_feature_get_boolean(map, "ReverseX", &value));
        EXPECT(value == values[i]);
    }
}

static void test_string(iris_node_map_t *map)
{
    iris_feature_info_t info;
    OK(iris_feature_info(map, "DeviceUserID", &info));
    EXPECT(info.type == IRIS_FEATURE_STRING && info.access == IRIS_FEATURE_READ_WRITE);
    const char *values[] = {"sim-camera-01", "triggered", ""};
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        OK(iris_feature_set_string(map, "DeviceUserID", values[i]));
        size_t size = 0;
        OK(iris_feature_get_string(map, "DeviceUserID", NULL, &size));
        EXPECT(size == strlen(values[i]) + 1);
        char value[16];
        OK(iris_feature_get_string(map, "DeviceUserID", value, &size));
        EXPECT(strcmp(value, values[i]) == 0);
    }
}

static void test_numeric(iris_node_map_t *map)
{
    iris_integer_range_t ir;
    iris_float_range_t fr;
    OK(iris_feature_integer_range(map, "Width", &ir));
    EXPECT(ir.minimum == 16 && ir.maximum == 64 && ir.increment == 16);
    EXPECT(ir.increment_mode == IRIS_INCREMENT_FIXED);
    OK(iris_feature_float_range(map, "ExposureTime", &fr));
    EXPECT(fr.minimum == 100 && fr.maximum == 1000 && fr.increment == 50);
    EXPECT(fr.increment_mode == IRIS_INCREMENT_FIXED);

    int64_t width;
    double exposure;
    OK(iris_feature_set_integer(map, "Width", 48));
    OK(iris_feature_get_integer(map, "Width", &width));
    EXPECT(width == 48);
    OK(iris_feature_set_float(map, "ExposureTime", 450));
    OK(iris_feature_get_float(map, "ExposureTime", &exposure));
    EXPECT(exposure == 450);
    EXPECT(iris_feature_set_integer(map, "Width", 80) == IRIS_OUT_OF_RANGE);
    EXPECT(iris_feature_set_float(map, "ExposureTime", 1050) == IRIS_OUT_OF_RANGE);
    OK(iris_feature_get_integer(map, "Width", &width));
    OK(iris_feature_get_float(map, "ExposureTime", &exposure));
    EXPECT(width == 48 && exposure == 450);

    char text[32];
    size_t size = sizeof(text);
    OK(iris_feature_unit(map, "Width", text, &size));
    EXPECT(strcmp(text, "px") == 0);
    size = sizeof(text);
    OK(iris_feature_unit(map, "ExposureTime", text, &size));
    EXPECT(strcmp(text, "us") == 0);
    size = sizeof(text);
    OK(iris_feature_to_string(map, "Width", text, &size));
    EXPECT(strcmp(text, "48") == 0);
    OK(iris_feature_from_string(map, "Width", "32"));
    OK(iris_feature_get_integer(map, "Width", &width));
    EXPECT(width == 32);

    size_t count = 0;
    OK(iris_feature_integer_values(map, "WidthRaw", NULL, &count));
    EXPECT(count == 3);
    int64_t valid[3] = {0};
    count = 2;
    EXPECT(iris_feature_integer_values(map, "WidthRaw", valid, &count)
           == IRIS_BUFFER_TOO_SMALL);
    EXPECT(count == 3 && valid[0] == 0);
    OK(iris_feature_integer_values(map, "WidthRaw", valid, &count));
    EXPECT(valid[0] == 16 && valid[1] == 32 && valid[2] == 64);
    count = 0;
    EXPECT(iris_feature_integer_values(map, "Width", NULL, &count) == IRIS_NOT_SUPPORTED);
    OK(iris_feature_float_values(map, "WidthScaled", NULL, &count));
    EXPECT(count == 3);
    double float_valid[3] = {0};
    count = 2;
    EXPECT(iris_feature_float_values(map, "WidthScaled", float_valid, &count)
           == IRIS_BUFFER_TOO_SMALL);
    EXPECT(count == 3 && float_valid[0] == 0);
    OK(iris_feature_float_values(map, "WidthScaled", float_valid, &count));
    EXPECT(float_valid[0] == 1 && float_valid[1] == 2 && float_valid[2] == 4);
    count = 0;
    EXPECT(iris_feature_float_values(map, "ExposureTime", NULL, &count) == IRIS_NOT_SUPPORTED);
    EXPECT(iris_feature_get_float(map, "Width", &exposure) == IRIS_WRONG_TYPE);
    EXPECT(iris_feature_get_integer(map, "NoSuchNode", &width) == IRIS_NOT_FOUND);
    EXPECT(iris_feature_set_integer(map, "TriggerCount", 2) == IRIS_ACCESS_DENIED);
}

static void test_enumeration_entries(iris_node_map_t *map)
{
    size_t count;
    OK(iris_feature_enumeration_count(map, "TriggerSelector", &count));
    EXPECT(count == 2);
    unsigned entries = 0;
    for (size_t i = 0; i < count; ++i) {
        char symbol[32] = "unchanged";
        iris_feature_access_t access;
        size_t size = 1;
        EXPECT(iris_feature_enumeration_entry(map, "TriggerSelector", i, symbol, &size, &access)
               == IRIS_BUFFER_TOO_SMALL);
        EXPECT(strcmp(symbol, "unchanged") == 0);
        OK(iris_feature_enumeration_entry(map, "TriggerSelector", i, symbol, &size, &access));
        EXPECT(access == IRIS_FEATURE_READ_ONLY);
        if (strcmp(symbol, "FrameStart") == 0) entries |= 1;
        if (strcmp(symbol, "ExposureStart") == 0) entries |= 2;
    }
    EXPECT(entries == 3);
}

static void test_command(iris_node_map_t *map)
{
    iris_feature_info_t info;
    OK(iris_feature_info(map, "TriggerSoftware", &info));
    EXPECT(info.type == IRIS_FEATURE_COMMAND);
    int64_t before, after;
    OK(iris_feature_get_integer(map, "TriggerCount", &before));
    OK(iris_feature_execute(map, "TriggerSoftware"));
    uint32_t done;
    OK(iris_feature_command_done(map, "TriggerSoftware", &done));
    EXPECT(done == 0);
    OK(iris_feature_command_done(map, "TriggerSoftware", &done));
    EXPECT(done == 1);
    OK(iris_feature_get_integer(map, "TriggerCount", &after));
    EXPECT(after == before + 1);
}

static void test_manual_invalidation(iris_node_map_t *map)
{
    EXPECT(iris_node_map_invalidate(NULL) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_feature_invalidate(map, "Missing") == IRIS_NOT_FOUND);
    iris_integer_range_t width, height;
    OK(iris_feature_integer_range(map, "Width", &width));
    OK(iris_feature_integer_range(map, "Height", &height));
    EXPECT(width.maximum == 64 && height.maximum == 64);
    OK(iris_feature_execute(map, "ChangeWidthMax"));
    OK(iris_feature_execute(map, "ChangeHeightMax"));
    OK(iris_feature_integer_range(map, "Width", &width));
    OK(iris_feature_integer_range(map, "Height", &height));
    EXPECT(width.maximum == 64 && height.maximum == 64);

    OK(iris_feature_invalidate(map, "WidthMaxReg"));
    OK(iris_feature_integer_range(map, "Width", &width));
    OK(iris_feature_integer_range(map, "Height", &height));
    EXPECT(width.maximum == 48 && height.maximum == 64);
    OK(iris_node_map_invalidate(map));
    OK(iris_feature_integer_range(map, "Height", &height));
    EXPECT(height.maximum == 32);

    OK(iris_feature_execute(map, "ChangeWidthMax"));
    OK(iris_feature_execute(map, "ChangeHeightMax"));
    OK(iris_node_map_invalidate(map));
    OK(iris_feature_integer_range(map, "Width", &width));
    OK(iris_feature_integer_range(map, "Height", &height));
    EXPECT(width.maximum == 64 && height.maximum == 64);
}

static void test_invalidation(iris_node_map_t *map)
{
    size_t processed = 99;
    EXPECT(iris_node_map_poll_invalidation(map, 1, &processed) == IRIS_INVALID_STATE);
    EXPECT(processed == 0);
    OK(iris_node_map_enable_invalidation(map, 1));
    OK(iris_node_map_enable_invalidation(map, 1));
    OK(iris_node_map_poll_invalidation(map, 1, &processed));
    EXPECT(processed == 0);

    iris_integer_range_t range;
    OK(iris_feature_integer_range(map, "Width", &range));
    EXPECT(range.maximum == 64);
    OK(iris_feature_execute(map, "ChangeWidthMax"));
    OK(iris_feature_integer_range(map, "Width", &range));
    EXPECT(range.maximum == 64);
    OK(iris_node_map_poll_invalidation(map, 1, &processed));
    EXPECT(processed == 1);
    OK(iris_feature_integer_range(map, "Width", &range));
    EXPECT(range.maximum == 48);
    OK(iris_node_map_poll_invalidation(map, 1, &processed));
    EXPECT(processed == 0);
    OK(iris_node_map_enable_invalidation(map, 0));
    EXPECT(iris_node_map_poll_invalidation(map, 1, &processed) == IRIS_INVALID_STATE);
    // Leave a subscription for device close to unregister.
    OK(iris_node_map_enable_invalidation(map, 1));
}

static void test_metadata(iris_node_map_t *map, iris_feature_metadata_t *selector, char *storage, size_t capacity)
{
    size_t size = 0;
    iris_feature_metadata_t metadata;
    EXPECT(iris_feature_metadata(map, "Missing", &metadata, NULL, &size) == IRIS_NOT_FOUND && size == 0);
    EXPECT(iris_feature_metadata(map, "ExposureTime", NULL, NULL, &size) == IRIS_INVALID_ARGUMENT);
    OK(iris_feature_metadata(map, "ExposureTime", &metadata, NULL, &size));
    EXPECT(size > strlen("ExposureTime") && !metadata.feature.name && !metadata.feature.display_name &&
           !metadata.feature.description && !metadata.feature.tooltip);
    EXPECT(metadata.feature.info.type == IRIS_FEATURE_FLOAT);
    EXPECT(metadata.representation == IRIS_REPRESENTATION_LOGARITHMIC);
    EXPECT(metadata.display_notation == IRIS_DISPLAY_NOTATION_FIXED && metadata.display_precision == 2);
    size = 0;
    OK(iris_feature_metadata(map, "WidthRaw", &metadata, NULL, &size));
    EXPECT(metadata.feature.info.type == IRIS_FEATURE_INTEGER);
    EXPECT(metadata.representation == IRIS_REPRESENTATION_HEX_NUMBER);
    EXPECT(metadata.display_notation == IRIS_DISPLAY_NOTATION_UNSPECIFIED && metadata.display_precision == -1);

    char small[] = "untouched";
    size = sizeof small;
    EXPECT(iris_feature_metadata(map, "TriggerSelector", &metadata, small, &size) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(strcmp(small, "untouched") == 0 && size > sizeof small &&
           metadata.feature.info.type == IRIS_FEATURE_INTEGER);
    EXPECT(size <= capacity);
    OK(iris_feature_metadata(map, "TriggerSelector", selector, storage, &size));
    EXPECT(selector->feature.name == storage);
    EXPECT(selector->feature.info.type == IRIS_FEATURE_ENUMERATION &&
           selector->feature.info.access == IRIS_FEATURE_READ_WRITE);
    EXPECT(selector->feature.visibility == IRIS_VISIBILITY_EXPERT);
    EXPECT(selector->representation == IRIS_REPRESENTATION_UNSPECIFIED &&
           selector->display_notation == IRIS_DISPLAY_NOTATION_UNSPECIFIED &&
           selector->display_precision == -1);
}

static iris_feature_list_t *test_browse(iris_node_map_t *map)
{
    iris_feature_list_t *list = NULL;
    iris_feature_entry_t entry;
    EXPECT(iris_feature_list_count(NULL) == 0);
    EXPECT(iris_node_map_browse(map, NULL, IRIS_SELECTED_FEATURES, &list) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_node_map_browse(map, "Width", 0, &list) == IRIS_INVALID_ARGUMENT);
    EXPECT(iris_node_map_browse(map, "Missing", IRIS_CATEGORY_FEATURES, &list) == IRIS_NOT_FOUND);
    EXPECT(iris_node_map_browse(map, "Width", IRIS_CATEGORY_FEATURES, &list) == IRIS_WRONG_TYPE);
    EXPECT(!list);
    OK(iris_node_map_browse(map, NULL, IRIS_CATEGORY_FEATURES, &list));
    OK(iris_feature_list_get(list, 0, &entry));
    EXPECT(strcmp(entry.name, "AcquisitionControl") == 0 && entry.info.type == IRIS_FEATURE_CATEGORY);
    EXPECT(iris_feature_list_get(list, iris_feature_list_count(list), &entry) == IRIS_INVALID_ARGUMENT);
    iris_feature_list_close(&list);
    OK(iris_node_map_browse(map, "TriggerSelector", IRIS_SELECTED_FEATURES, &list));
    EXPECT(iris_feature_list_count(list) == 1);
    OK(iris_feature_list_get(list, 0, &entry));
    EXPECT(strcmp(entry.name, "TriggerMode") == 0);
    iris_feature_list_close(&list);
    OK(iris_node_map_browse(map, "TriggerMode", IRIS_SELECTING_FEATURES, &list));
    EXPECT(iris_feature_list_count(list) == 1);
    OK(iris_feature_list_get(list, 0, &entry));
    EXPECT(strcmp(entry.name, "TriggerSelector") == 0);
    iris_feature_list_close(&list);
    OK(iris_node_map_browse(map, "AcquisitionControl", IRIS_CATEGORY_FEATURES, &list));
    EXPECT(iris_feature_list_count(list) == 2);
    return list;
}

static void test_xml_url(const char *cti, const char *scenario, uint32_t index, uint32_t indexed,
                         const char *prefix, int64_t width)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *map = NULL;
    const iris_device_selector_t selector = {cti, "test-interface", "test-device", NULL};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_READ_ONLY, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    iris_node_map_xml_info_t info;
    OK(iris_node_map_xml_info(map, &info));
    EXPECT(info.indexed == indexed && info.url_index == index);
    EXPECT(strncmp(info.url, prefix, strlen(prefix)) == 0);
    if (strcmp(scenario, "xml-indexed") == 0) {
        EXPECT(info.valid == (IRIS_XML_HAS_SCHEMA_VERSION | IRIS_XML_HAS_FILE_VERSION));
        EXPECT(info.schema_major == 1 && info.schema_minor == 1);
        EXPECT(info.file_major == 1 && info.file_minor == 0 && info.file_subminor == 0);
        unsigned char value[4] = {1, 0, 0, 0};
        EXPECT(iris_feature_write_register(map, "TriggerModeReg", value, sizeof value) == IRIS_ACCESS_DENIED);
    } else {
        EXPECT(info.valid == 0);
    }
    int64_t actual;
    OK(iris_feature_get_integer(map, "Width", &actual));
    EXPECT(actual == width);
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
}

static void test_file_url(const char *cti)
{
    char directory[] = "/tmp/iris-xml-XXXXXX";
    EXPECT(mkdtemp(directory));
    char path[128], url[160];
    snprintf(path, sizeof path, "%s/node fixture.XML", directory);
    FILE *file = fopen(path, "w");
    EXPECT(file);
    EXPECT(fputs(
        "<RegisterDescription xmlns=\"http://www.genicam.org/GenApi/Version_1_1\" "
        "ModelName=\"FileFixture\" VendorName=\"IrisTests\" ToolTip=\"File XML fixture\" "
        "StandardNameSpace=\"None\" SchemaMajorVersion=\"1\" SchemaMinorVersion=\"1\" SchemaSubMinorVersion=\"0\" "
        "MajorVersion=\"1\" MinorVersion=\"0\" SubMinorVersion=\"0\" "
        "ProductGuid=\"caf6e7e5-2abc-43df-b04a-70102901c1c8\" VersionGuid=\"deaaefdd-ce58-446d-bb28-4b0d94b13427\">"
        "<Category Name=\"Root\"><pFeature>Width</pFeature></Category>"
        "<Integer Name=\"Width\"><Value>32</Value></Integer>"
        "<Port Name=\"Device\"/></RegisterDescription>", file) >= 0);
    EXPECT(fclose(file) == 0);
    snprintf(url, sizeof url, "file://localhost%s/node%%20fixture.XML?SchemaVersion=1.1.0", directory);
    EXPECT(setenv("IRIS_TEST_XML_URL", url, 1) == 0);
    test_xml_url(cti, "xml-file", 0, 1, url, 32);
    EXPECT(unlink(path) == 0 && rmdir(directory) == 0 && unsetenv("IRIS_TEST_XML_URL") == 0);
}

static void test_producer_contract(const char *cti, const char *scenario)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *map = NULL;
    const iris_device_selector_t selector = {cti, "test-interface", "test-device", NULL};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    if (strcmp(scenario, "port-access") == 0) {
        iris_feature_metadata_t metadata;
        size_t size = 0;
        OK(iris_feature_metadata(map, "RawData", &metadata, NULL, &size));
        EXPECT(metadata.feature.info.access == IRIS_FEATURE_READ_ONLY);
        unsigned char data[4];
        size = sizeof data;
        OK(iris_feature_read_register(map, "RawData", data, &size));
        EXPECT(iris_feature_write_register(map, "RawData", data, sizeof data) == IRIS_ACCESS_DENIED);
    } else if (strcmp(scenario, "invalidation-malformed") == 0) {
        OK(iris_node_map_enable_invalidation(map, 1));
        OK(iris_feature_execute(map, "ChangeWidthMax"));
        size_t processed;
        EXPECT(iris_node_map_poll_invalidation(map, 1, &processed) == IRIS_PRODUCER_ERROR && processed == 1);
        OK(iris_node_map_poll_invalidation(map, 1, &processed));
        EXPECT(processed == 0);
    } else if (strcmp(scenario, "invalidation-oversized") == 0) {
        OK(iris_node_map_enable_invalidation(map, 1));
        iris_integer_range_t range;
        OK(iris_feature_integer_range(map, "Width", &range));
        EXPECT(range.maximum == 64);
        OK(iris_feature_execute(map, "ChangeWidthMax"));
        size_t processed;
        EXPECT(iris_node_map_poll_invalidation(map, 1, &processed) == IRIS_OUT_OF_RANGE && processed == 1);
        OK(iris_feature_integer_range(map, "Width", &range));
        EXPECT(range.maximum == 48);
        OK(iris_feature_execute(map, "ChangeWidthMax"));
        OK(iris_node_map_poll_invalidation(map, 1, &processed));
        EXPECT(processed == 1);
        OK(iris_feature_integer_range(map, "Width", &range));
        EXPECT(range.maximum == 64);
    } else {
        EXPECT(iris_node_map_enable_invalidation(map, 1) == IRIS_PRODUCER_ERROR);
        iris_error_t error;
        iris_last_error(&error);
        EXPECT(strcmp(error.operation, "GCRegisterEvent") == 0 && error.native_code_valid);
        EXPECT(iris_node_map_enable_invalidation(map, 1) == IRIS_INVALID_STATE);
        size_t processed;
        EXPECT(iris_node_map_poll_invalidation(map, 1, &processed) == IRIS_INVALID_STATE && processed == 0);
    }
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    EXPECT(setenv("IRIS_FAULT", "nodes", 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *map = NULL;
    iris_device_selector_t selector = {argv[1], "test-interface", "test-device", NULL};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    iris_feature_list_t *features = test_browse(map);
    iris_feature_metadata_t metadata;
    char metadata_storage[128];
    test_metadata(map, &metadata, metadata_storage, sizeof metadata_storage);
    test_numeric(map);
    test_enumeration_entries(map);
    test_selector(map);
    test_registers(map);
    test_boolean(map);
    test_string(map);
    test_command(map);
    test_manual_invalidation(map);
    test_invalidation(map);
    iris_device_t *peer = NULL;
    selector.device_id = "test-device-2";
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &peer));
    iris_node_map_t *shared, *peer_map;
    OK(iris_device_node_map(device, IRIS_SYSTEM_MODULE, &shared));
    OK(iris_device_node_map(peer, IRIS_SYSTEM_MODULE, &peer_map));
    EXPECT(shared == peer_map);
    OK(iris_device_node_map(device, IRIS_INTERFACE_MODULE, &shared));
    OK(iris_device_node_map(peer, IRIS_INTERFACE_MODULE, &peer_map));
    EXPECT(shared == peer_map);
    OK(iris_feature_set_string(shared, "DeviceUserID", "shared"));
    char user_id[16];
    size_t size = sizeof(user_id);
    OK(iris_feature_get_string(peer_map, "DeviceUserID", user_id, &size));
    EXPECT(strcmp(user_id, "shared") == 0);
    OK(iris_node_map_enable_invalidation(map, 0));
    EXPECT(iris_node_map_enable_invalidation(shared, 1) == IRIS_NOT_SUPPORTED);
    OK(iris_device_close(&device));
    size = sizeof user_id;
    OK(iris_feature_get_string(peer_map, "DeviceUserID", user_id, &size));
    EXPECT(strcmp(user_id, "shared") == 0);
    OK(iris_device_close(&peer));
    OK(iris_context_close(&context));
    iris_feature_entry_t entry;
    OK(iris_feature_list_get(features, 0, &entry));
    EXPECT(strcmp(entry.name, "TriggerSelector") == 0);
    EXPECT(strcmp(entry.display_name, "Trigger Selector") == 0);
    EXPECT(strcmp(entry.description, "Selects the trigger to configure.") == 0);
    EXPECT(strcmp(entry.tooltip, "Selects a trigger.") == 0);
    EXPECT(entry.visibility == IRIS_VISIBILITY_EXPERT && entry.info.access == IRIS_FEATURE_READ_WRITE);
    EXPECT(strcmp(metadata.feature.name, entry.name) == 0);
    EXPECT(strcmp(metadata.feature.display_name, entry.display_name) == 0);
    EXPECT(strcmp(metadata.feature.description, entry.description) == 0);
    EXPECT(strcmp(metadata.feature.tooltip, entry.tooltip) == 0);
    iris_feature_list_close(&features);
    EXPECT(!features);
    test_xml_url(argv[1], "xml-indexed", 3, 1, "local:node-fixture.xml;1000;", 16);
    test_xml_url(argv[1], "xml-legacy", 1, 0, "local:node-fixture.xml;1000;", 16);
    test_file_url(argv[1]);
    test_producer_contract(argv[1], "invalidation-rollback");
    test_producer_contract(argv[1], "invalidation-malformed");
    test_producer_contract(argv[1], "invalidation-oversized");
    test_producer_contract(argv[1], "port-access");
}
