#define _POSIX_C_SOURCE 200809L
#include "iris.h"

#include <archive.h>
#include <archive_entry.h>

#include <fcntl.h>
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

static void expect_entry(const iris_feature_list_t *list, size_t index, const char *name,
                         iris_feature_access_t access)
{
    iris_feature_entry_t entry;
    OK(iris_feature_list_get(list, index, &entry));
    EXPECT(strcmp(entry.name, name) == 0 && entry.info.access == access);
}

static iris_feature_list_t *test_list(iris_node_map_t *map)
{
    iris_feature_list_t *list = NULL;
    iris_feature_filter_t options = {"Width*", IRIS_FEATURE_MATCH_GLOB, IRIS_FEATURE_INTEGER, 0};
    OK(iris_node_map_list_features(map, &options, &list));
    EXPECT(iris_feature_list_count(list) == 2);
    expect_entry(list, 0, "Width", IRIS_FEATURE_READ_WRITE);
    expect_entry(list, 1, "WidthUnavailable", IRIS_FEATURE_NOT_AVAILABLE);
    iris_feature_list_close(&list);

    options.flags = IRIS_FEATURE_LIST_AVAILABLE_ONLY;
    OK(iris_node_map_list_features(map, &options, &list));
    EXPECT(iris_feature_list_count(list) == 1);
    expect_entry(list, 0, "Width", IRIS_FEATURE_READ_WRITE);
    iris_feature_list_close(&list);

    options.flags |= IRIS_FEATURE_LIST_INCLUDE_INTERNAL;
    OK(iris_node_map_list_features(map, &options, &list));
    EXPECT(iris_feature_list_count(list) >= 3);
    expect_entry(list, 0, "Width", IRIS_FEATURE_READ_WRITE);
    expect_entry(list, 1, "WidthMaxReg", IRIS_FEATURE_READ_ONLY);
    expect_entry(list, 2, "WidthRaw", IRIS_FEATURE_READ_WRITE);
    iris_feature_list_close(&list);

    options = (iris_feature_filter_t){"^Trigger(Mode|Selector)$", IRIS_FEATURE_MATCH_REGEX, IRIS_FEATURE_ENUMERATION, 0};
    OK(iris_node_map_list_features(map, &options, &list));
    EXPECT(iris_feature_list_count(list) == 2);
    expect_entry(list, 0, "TriggerMode", IRIS_FEATURE_READ_WRITE);
    expect_entry(list, 1, "TriggerSelector", IRIS_FEATURE_READ_WRITE);
    iris_feature_list_close(&list);

    options.name_pattern = "[";
    EXPECT(iris_node_map_list_features(map, &options, &list) == IRIS_INVALID_ARGUMENT && !list);
    options = (iris_feature_filter_t){"does-not-match*", IRIS_FEATURE_MATCH_GLOB, 0, 0};
    OK(iris_node_map_list_features(map, &options, &list));
    EXPECT(list && iris_feature_list_count(list) == 0);
    iris_feature_list_close(&list);
    OK(iris_node_map_list_features(map, NULL, &list));
    EXPECT(iris_feature_list_count(list) > 2);
    return list;
}

static void test_details(iris_node_map_t *map)
{
    size_t size = 0;
    OK(iris_feature_alias(map, "Width", NULL, &size));
    EXPECT(size == sizeof "WidthRaw");
    char text[32] = "untouched";
    size = 2;
    EXPECT(iris_feature_alias(map, "Width", text, &size) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(size == sizeof "WidthRaw" && strcmp(text, "untouched") == 0);
    OK(iris_feature_alias(map, "Width", text, &size));
    EXPECT(strcmp(text, "WidthRaw") == 0);
    size = sizeof text;
    EXPECT(iris_feature_alias(map, "RawData", text, &size) == IRIS_NOT_FOUND);

    OK(iris_feature_event_id(map, "EventCounter", text, &size));
    EXPECT(strcmp(text, "1234") == 0);
    size = sizeof text;
    OK(iris_feature_event_id(map, "EventWideCounter", text, &size));
    EXPECT(strcmp(text, "112233445566778899") == 0);
    size = sizeof text;
    OK(iris_feature_event_id(map, "EventPort", text, &size));
    EXPECT(strcmp(text, "1234") == 0);
    EXPECT(iris_feature_event_id(map, "Width", text, &size) == IRIS_NOT_FOUND);
    EXPECT(iris_feature_event_id(map, "CombinedCounter", text, &size) == IRIS_AMBIGUOUS);
    size_t length;
    OK(iris_feature_string_max_length(map, "DeviceUserID", &length));
    EXPECT(length == 16);
    EXPECT(iris_feature_string_max_length(map, "Width", &length) == IRIS_WRONG_TYPE);
}

static char *read_xml(iris_node_map_t *map, size_t *size)
{
    *size = 0;
    OK(iris_node_map_xml(map, NULL, size));
    char bytes[3] = "ok";
    size_t capacity = sizeof bytes;
    EXPECT(iris_node_map_xml(map, bytes, &capacity) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(capacity == *size && strcmp(bytes, "ok") == 0);
    char *xml = malloc(*size);
    EXPECT(xml);
    OK(iris_node_map_xml(map, xml, size));
    EXPECT(xml[*size - 1] == '\0' && strstr(xml, "<RegisterDescription"));

    char fragment[1024];
    capacity = 0;
    OK(iris_feature_xml(map, "Width", NULL, &capacity));
    EXPECT(capacity <= sizeof fragment);
    OK(iris_feature_xml(map, "Width", fragment, &capacity));
    EXPECT(strstr(fragment, "<Integer") && strstr(fragment, "Name=\"Width\"") &&
           strstr(fragment, "<pAlias>WidthRaw</pAlias>") && strstr(fragment, "xmlns="));
    capacity = sizeof fragment;
    OK(iris_feature_xml(map, "FrameStart", fragment, &capacity));
    EXPECT(strstr(fragment, "<EnumEntry") && strstr(fragment, "Name=\"FrameStart\""));
    EXPECT(iris_feature_xml(map, "Missing", fragment, &capacity) == IRIS_NOT_FOUND);
    EXPECT(iris_feature_xml(map, "Off", fragment, &capacity) == IRIS_AMBIGUOUS);
    return xml;
}

static void write_zip(const char *xml, size_t size, int fd)
{
    struct archive *writer = archive_write_new();
    EXPECT(writer && archive_write_set_format_zip(writer) == ARCHIVE_OK);
    EXPECT(archive_write_zip_set_compression_deflate(writer) == ARCHIVE_OK);
    EXPECT(archive_write_open_fd(writer, fd) == ARCHIVE_OK);
    struct archive_entry *entry = archive_entry_new();
    EXPECT(entry);
    archive_entry_set_pathname(entry, "camera.xml");
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0600);
    archive_entry_set_size(entry, size);
    EXPECT(archive_write_header(writer, entry) == ARCHIVE_OK);
    EXPECT(archive_write_data(writer, xml, size) == (ssize_t)size);
    archive_entry_free(entry);
    EXPECT(archive_write_free(writer) == ARCHIVE_OK);
    EXPECT(close(fd) == 0);
}

static void test_xml_override(iris_context_t *context, const iris_device_selector_t *selector,
                              const char *xml, size_t size)
{
    iris_device_t *peer = NULL;
    iris_device_t *device = NULL;
    OK(iris_device_open(context, selector, IRIS_DEVICE_CONTROL, 0, &peer));
    iris_device_selector_t second = *selector;
    second.device_id = "test-device-2";

    iris_device_open_options_t options = {0, "", IRIS_DEVICE_READ_ONLY};
    EXPECT(iris_device_open_with_options(context, &second, NULL, &device) == IRIS_INVALID_ARGUMENT && !device);
    EXPECT(iris_device_open_with_options(context, &second, &options, &device) == IRIS_INVALID_ARGUMENT && !device);
    options.remote_xml_path = ".";
    EXPECT(iris_device_open_with_options(context, &second, &options, &device) == IRIS_IO_ERROR && !device);

    char path[] = "iris-override-XXXXXX";
    int fd = mkstemp(path);
    EXPECT(fd >= 0);
    options.remote_xml_path = path;
    EXPECT(iris_device_open_with_options(context, &second, &options, &device) == IRIS_OUT_OF_RANGE && !device);
    const char malformed[] = "<RegisterDescription";
    EXPECT(write(fd, malformed, sizeof malformed - 1) == sizeof malformed - 1 && close(fd) == 0);
    EXPECT(iris_device_open_with_options(context, &second, &options, &device) == IRIS_GENAPI_ERROR && !device);
    iris_error_t error;
    iris_last_error(&error);
    EXPECT(strcmp(error.operation, "GenApi XML override") == 0 && *error.message);

    fd = open(path, O_WRONLY | O_TRUNC);
    EXPECT(fd >= 0);
    write_zip(xml, size - 1, fd);
    OK(iris_device_open_with_options(context, &second, &options, &device));
    EXPECT(unlink(path) == 0);
    iris_node_map_t *map;
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    iris_node_map_xml_info_t source;
    OK(iris_node_map_xml_info(map, &source));
    EXPECT(source.valid == IRIS_XML_IS_OVERRIDE && !source.indexed && source.url_index == UINT32_MAX);
    EXPECT(strncmp(source.url, "file:///", 8) == 0 && strstr(source.url, path));
    size_t copied;
    char *bytes = read_xml(map, &copied);
    EXPECT(copied == size && memcmp(bytes, xml, size) == 0);
    free(bytes);
    EXPECT(iris_feature_set_enumeration(map, "TriggerMode", "On") == IRIS_ACCESS_DENIED);
    OK(iris_device_node_map(device, IRIS_SYSTEM_MODULE, &map));
    OK(iris_node_map_xml_info(map, &source));
    EXPECT(!(source.valid & IRIS_XML_IS_OVERRIDE));
    OK(iris_device_node_map(peer, IRIS_REMOTE_DEVICE, &map));
    OK(iris_node_map_xml_info(map, &source));
    EXPECT(!(source.valid & IRIS_XML_IS_OVERRIDE));
    OK(iris_device_close(&device));
    EXPECT(iris_device_open_with_options(context, &second, &options, &device) == IRIS_IO_ERROR && !device);
    OK(iris_device_close(&peer));
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    const iris_device_selector_t selector = {argv[1], "test-interface", "test-device", NULL};
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *map = NULL;
    EXPECT(setenv("IRIS_FAULT", "nodes", 1) == 0);
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    iris_feature_list_t *snapshot = test_list(map);
    test_details(map);
    size_t size;
    char *xml = read_xml(map, &size);
    OK(iris_device_close(&device));
    iris_feature_entry_t first;
    OK(iris_feature_list_get(snapshot, 0, &first));
    EXPECT(strcmp(first.name, "AcquisitionControl") == 0);
    iris_feature_list_close(&snapshot);
    test_xml_override(context, &selector, xml, size);

    char path[] = "/tmp/iris-feature-xml-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT(fd >= 0);
    write_zip(xml, size - 1, fd);
    char url[128];
    snprintf(url, sizeof url, "file://%s", path);
    EXPECT(setenv("IRIS_FAULT", "xml-file", 1) == 0 && setenv("IRIS_TEST_XML_URL", url, 1) == 0);
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &map));
    EXPECT(unlink(path) == 0);
    size_t zipped_size;
    char *zipped_xml = read_xml(map, &zipped_size);
    EXPECT(zipped_size == size && memcmp(zipped_xml, xml, size) == 0);
    free(zipped_xml);
    free(xml);
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
}
