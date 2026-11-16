#define _POSIX_C_SOURCE 200809L
#include "iris.h"
#include "allocations.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return;
    iris_error_t error;
    iris_last_error(&error);
    fprintf(stderr, "%s: %d (%s)\n", call, status, error.message);
    exit(1);
}
#define OK(call) check((call), #call)

static void poll_parts(void *data, const iris_frame_t *frame)
{
    size_t *count = data;
    EXPECT(frame->parts_status == IRIS_OK && frame->parts && frame->part_count == 2);
    EXPECT(frame->parts[1].size == 8 && frame->parts[1].width == 4);
    *count += frame->part_count;
}

static iris_feature_type_t chunk_type(const iris_feature_list_t *list, const char *name)
{
    const char *previous = NULL;
    iris_feature_type_t type = 0;
    for (size_t i = 0; i < iris_feature_list_count(list); ++i) {
        iris_feature_entry_t entry;
        OK(iris_feature_list_get(list, i, &entry));
        EXPECT(!previous || strcmp(previous, entry.name) < 0);
        EXPECT(entry.info.access == IRIS_FEATURE_READ_ONLY);
        if (!strcmp(entry.name, name)) type = entry.info.type;
        previous = entry.name;
    }
    return type;
}

static void test_typed_chunks(iris_stream_t *stream, const iris_frame_t *frame, const iris_feature_list_t *chunks)
{
    EXPECT(chunk_type(chunks, "ChunkTemperature") == IRIS_FEATURE_FLOAT);
    EXPECT(chunk_type(chunks, "ChunkLabel") == IRIS_FEATURE_STRING);
    EXPECT(chunk_type(chunks, "ChunkEnabled") == IRIS_FEATURE_BOOLEAN);
    EXPECT(chunk_type(chunks, "ChunkChoice") == IRIS_FEATURE_ENUMERATION);
    EXPECT(chunk_type(chunks, "ChunkRaw") == IRIS_FEATURE_REGISTER);
    EXPECT(chunk_type(chunks, "ChunkWide") == IRIS_FEATURE_INTEGER);
    EXPECT(chunk_type(chunks, "ChunkFlagsReg") == IRIS_FEATURE_INTEGER);
    EXPECT(!chunk_type(chunks, "ChunkMixed"));

    double temperature;
    uint32_t enabled;
    int64_t wide;
    OK(iris_frame_chunk_get_float(stream, frame, "ChunkTemperature", &temperature));
    OK(iris_frame_chunk_get_boolean(stream, frame, "ChunkEnabled", &enabled));
    OK(iris_frame_chunk_get_integer(stream, frame, "ChunkWide", &wide));
    EXPECT(temperature == 12.5 && enabled == 1 && wide == INT64_C(9007199254740993));

    char text[16] = "unchanged";
    size_t size = 0;
    OK(iris_frame_chunk_get_string(stream, frame, "ChunkLabel", NULL, &size));
    EXPECT(size == 4);
    size = 2;
    EXPECT(iris_frame_chunk_get_string(stream, frame, "ChunkLabel", text, &size) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(size == 4 && strcmp(text, "unchanged") == 0);
    OK(iris_frame_chunk_get_string(stream, frame, "ChunkLabel", text, &size));
    EXPECT(strcmp(text, "sim") == 0);
    size = sizeof text;
    OK(iris_frame_chunk_get_enumeration(stream, frame, "ChunkChoice", text, &size));
    EXPECT(size == sizeof "Second" && strcmp(text, "Second") == 0);

    unsigned char raw[] = {0xa5, 0xa5, 0xa5, 0xa5};
    size = 0;
    OK(iris_frame_chunk_read_register(stream, frame, "ChunkRaw", NULL, &size));
    EXPECT(size == sizeof raw);
    size = 2;
    EXPECT(iris_frame_chunk_read_register(stream, frame, "ChunkRaw", raw, &size) == IRIS_BUFFER_TOO_SMALL);
    EXPECT(size == sizeof raw && raw[0] == 0xa5);
    OK(iris_frame_chunk_read_register(stream, frame, "ChunkRaw", raw, &size));
    const unsigned char expected[] = {0xde, 0xad, 0, 0xff};
    EXPECT(size == sizeof expected && memcmp(raw, expected, size) == 0);
    EXPECT(iris_frame_chunk_get_float(stream, frame, "ChunkWide", &temperature) == IRIS_WRONG_TYPE);
    EXPECT(iris_frame_chunk_get_integer(stream, frame, "Width", &wide) == IRIS_WRONG_TYPE);
    EXPECT(iris_frame_chunk_get_integer(stream, frame, "ChunkMixed", &wide) == IRIS_NOT_FOUND);
}

static void run(const char *path, const char *scenario)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_stream_t *stream = NULL;
    const iris_device_selector_t selector = {path, NULL, NULL, "test-serial"};
    allocation_begin();
    OK(iris_context_create(&context));
    const allocation_counts calibration = allocation_end();
    EXPECT(!IRIS_TEST_ALLOCATIONS || calibration.consumer_calls > 0);
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_stream_open(device, 0, &stream));
    _Alignas(64) unsigned char memory[288] = {0};
    const iris_buffer_t buffer = {memory, sizeof memory, NULL};
    const iris_start_options_t options = {1, IRIS_ACQUISITION_STREAM_ONLY, &buffer};
    OK(iris_stream_start(stream, &options));
    iris_frame_t frame;
    allocation_begin();
    OK(iris_stream_try_get_frame(stream, &frame));
    const allocation_counts counts = allocation_end();
    EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
    if (frame.payload.type == IRIS_PAYLOAD_MULTI_PART || frame.payload.type == IRIS_PAYLOAD_GENDC ||
        !strcmp(scenario, "chunk-data-size") || !strcmp(scenario, "chunk-bounds-data-size"))
        EXPECT(!frame.payload.data);
    else
        EXPECT(frame.payload.data == memory);
    const int gapped_multipart = !strcmp(scenario, "multipart-gap") || !strcmp(scenario, "multipart-no-size") ||
        !strcmp(scenario, "multipart-chunk") || !strcmp(scenario, "multipart-chunk-overlap");
    if (!strcmp(scenario, "multipart") || gapped_multipart || !strcmp(scenario, "multipart-duplicate")) {
        EXPECT(frame.part_count == 2 && frame.parts_status == IRIS_OK);
        EXPECT((uintptr_t)frame.parts % 64 == 0);
        EXPECT(frame.parts[0].data == memory &&
               frame.parts[1].data == memory + (gapped_multipart ? 24 : 8));
        EXPECT(frame.parts[0].format == 0x01080001 && frame.parts[0].height == 2);
        if (!strcmp(scenario, "multipart-duplicate")) {
            iris_frame_t duplicate;
            EXPECT(iris_stream_try_get_frame(stream, &duplicate) == IRIS_PRODUCER_ERROR);
            EXPECT(!duplicate.token && frame.parts[1].data == memory + 8 && frame.parts[1].size == 8);
            OK(iris_stream_stop(stream));
            EXPECT(frame.parts[1].data == memory + 8);
            EXPECT(iris_stream_close(&stream) == IRIS_BUSY);
        }
    } else if (!strcmp(scenario, "gendc") || !strcmp(scenario, "gendc-chunk") ||
               !strcmp(scenario, "gendc-chunk-duplicate") ||
               !strcmp(scenario, "gendc-gap") || !strcmp(scenario, "gendc-custom")) {
        EXPECT(frame.part_count == 1 && frame.parts_status == IRIS_OK);
        EXPECT(frame.parts[0].data == memory + (!strcmp(scenario, "gendc-gap") ? 192 : 184) &&
               frame.parts[0].source_id == 7 &&
               frame.parts[0].region_id == 9);
        if (!strcmp(scenario, "gendc"))
            EXPECT(frame.parts[0].type == 0x4200 && frame.parts[0].component_type == 1 &&
                   frame.parts[0].width == 4 && frame.parts[0].height == 2);
        if (!strcmp(scenario, "gendc-custom"))
            EXPECT(frame.parts[0].type == 0x42f0 && !(frame.parts[0].valid & IRIS_PART_HAS_WIDTH));
    } else if (!strcmp(scenario, "gendc-xml")) {
        EXPECT(frame.part_count == 2 && frame.parts_status == IRIS_OK);
        EXPECT(frame.parts[0].data == memory + 256 && frame.parts[1].data == memory + 272);
    } else if (!strcmp(scenario, "multipart-chunk-only")) {
        EXPECT(frame.part_count == 0 && !frame.parts && frame.parts_status == IRIS_OK);
    } else if (frame.payload.type == IRIS_PAYLOAD_GENDC || frame.payload.type == IRIS_PAYLOAD_MULTI_PART) {
        EXPECT(frame.part_count == 0 && !frame.parts && frame.parts_status < 0);
    }
    size_t size = 0;
    iris_feature_list_t *chunk_features = NULL;
    if (!strcmp(scenario, "chunk") || !strcmp(scenario, "chunk-typed") ||
        !strcmp(scenario, "chunk-data-size") || !strcmp(scenario, "gendc-chunk") ||
        !strcmp(scenario, "multipart-chunk") || !strcmp(scenario, "multipart-chunk-only")) {
        OK(iris_frame_chunk_list(stream, &frame, &chunk_features));
        EXPECT(chunk_type(chunk_features, "ChunkCounter") == IRIS_FEATURE_INTEGER);
        EXPECT(!chunk_type(chunk_features, "Root") && !chunk_type(chunk_features, "Width") &&
               !chunk_type(chunk_features, "ChunkMissing"));
        int64_t counter;
        OK(iris_frame_chunk_get_integer(stream, &frame, "ChunkCounter", &counter));
        EXPECT(counter == 42);
        EXPECT(iris_frame_chunk_get_integer(stream, &frame, "ChunkMissing", &counter) == IRIS_NOT_FOUND);
        if (!strcmp(scenario, "chunk-typed")) {
            test_typed_chunks(stream, &frame, chunk_features);
            iris_node_map_t *remote;
            OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote));
            EXPECT(iris_feature_get_integer(remote, "ChunkWide", &counter) < 0);
            OK(iris_feature_get_integer(remote, "Width", &counter));
            EXPECT(counter == 16);
        }

        char value[8];
        OK(iris_frame_chunk_to_string(stream, &frame, "ChunkCounter", NULL, &size));
        EXPECT(size == 3);
        size = 1;
        EXPECT(iris_frame_chunk_to_string(stream, &frame, "ChunkCounter", value, &size) == IRIS_BUFFER_TOO_SMALL);
        OK(iris_frame_chunk_to_string(stream, &frame, "ChunkCounter", value, &size));
        EXPECT(!strcmp(value, "42"));
        EXPECT(iris_frame_chunk_to_string(stream, &frame, "Width", value, &size) == IRIS_WRONG_TYPE);
        if (!strcmp(scenario, "gendc-chunk")) {
            OK(iris_stream_stop(stream));
            OK(iris_frame_chunk_get_integer(stream, &frame, "ChunkCounter", &counter));
            EXPECT(counter == 42);
        }
    } else if (!strcmp(scenario, "bad-chunk") || !strcmp(scenario, "chunk-bounds-data-size") ||
               !strcmp(scenario, "multipart-chunk-overlap") ||
               !strcmp(scenario, "chunk-duplicate") || !strcmp(scenario, "chunk-overlap") ||
               !strcmp(scenario, "gendc-chunk-duplicate")) {
        EXPECT(iris_frame_chunk_to_string(stream, &frame, "ChunkCounter", NULL, &size) == IRIS_PRODUCER_ERROR);
        EXPECT(iris_frame_chunk_list(stream, &frame, &chunk_features) == IRIS_PRODUCER_ERROR && !chunk_features);
    } else if (!strcmp(scenario, "gendc-xml")) {
        EXPECT(iris_frame_chunk_to_string(stream, &frame, "ChunkCounter", NULL, &size) == IRIS_NOT_SUPPORTED);
    } else if (!strcmp(scenario, "multipart")) {
        EXPECT(iris_frame_chunk_list(stream, &frame, &chunk_features) == IRIS_NOT_SUPPORTED && !chunk_features);
    }
    const iris_frame_t stale = frame;
    allocation_begin();
    OK(iris_stream_release_frame(stream, &frame));
    if (!strcmp(scenario, "multipart")) {
        size_t parts = 0, delivered = 0;
        OK(iris_stream_poll(stream, poll_parts, &parts, 1, &delivered));
        EXPECT(parts == 2 && delivered == 1);
    }
    const allocation_counts released = allocation_end();
    EXPECT(released.consumer_calls == 0 && released.consumer_bytes == 0);
    size = 0;
    EXPECT(iris_frame_chunk_to_string(stream, &stale, "ChunkCounter", NULL, &size) == IRIS_INVALID_ARGUMENT);
    iris_feature_list_t *next_chunks = NULL;
    EXPECT(iris_frame_chunk_list(stream, &stale, &next_chunks) == IRIS_INVALID_ARGUMENT && !next_chunks);
    if (!strcmp(scenario, "chunk-typed")) {
        allocation_begin();
        OK(iris_stream_try_get_frame(stream, &frame));
        EXPECT(allocation_end().consumer_calls == 0);
        OK(iris_frame_chunk_list(stream, &frame, &next_chunks));
        EXPECT(iris_feature_list_count(next_chunks) == 1);
        EXPECT(chunk_type(next_chunks, "ChunkCounter") == IRIS_FEATURE_INTEGER);
        int64_t counter;
        OK(iris_frame_chunk_get_integer(stream, &frame, "ChunkCounter", &counter));
        EXPECT(counter == 43);
        EXPECT(iris_frame_chunk_get_integer(stream, &stale, "ChunkCounter", &counter) == IRIS_INVALID_ARGUMENT);
        double temperature;
        EXPECT(iris_frame_chunk_get_float(stream, &frame, "ChunkTemperature", &temperature) == IRIS_NOT_FOUND);
        iris_feature_list_close(&next_chunks);
        allocation_begin();
        OK(iris_stream_release_frame(stream, &frame));
        EXPECT(allocation_end().consumer_calls == 0);
    }
    OK(iris_stream_stop(stream));
    OK(iris_stream_close(&stream));
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
    if (chunk_features) {
        EXPECT(chunk_type(chunk_features, "ChunkCounter") == IRIS_FEATURE_INTEGER);
        if (!strcmp(scenario, "chunk-typed")) EXPECT(chunk_type(chunk_features, "ChunkTemperature") == IRIS_FEATURE_FLOAT);
        iris_feature_list_close(&chunk_features);
    }
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    const char *cases[] = {"chunk", "chunk-typed", "bad-chunk", "chunk-duplicate", "chunk-overlap", "chunk-data-size", "chunk-bounds-data-size",
                          "multipart", "multipart-count", "multipart-bounds",
                          "multipart-composite", "multipart-gap", "multipart-duplicate",
                          "multipart-overlap", "multipart-no-status", "multipart-no-size",
                          "multipart-chunk", "multipart-chunk-overlap", "multipart-chunk-only",
                          "gendc", "gendc-version", "gendc-bounds", "gendc-chunk", "gendc-gap",
                          "gendc-prefetch", "gendc-custom", "gendc-xml", "gendc-alias",
                          "gendc-chunk-duplicate", "gendc-h264-short"};
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) run(argv[1], cases[i]);
    return 0;
}
