#define _POSIX_C_SOURCE 200809L
#include "allocations.h"
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define OK(call) EXPECT((call) == IRIS_OK)

typedef struct {
    iris_device_t *device;
    iris_stream_t *stream;
} camera_t;

static camera_t open_camera(iris_context_t *context, const char *cti, const char *mode)
{
    camera_t camera = {0};
    EXPECT(setenv("IRIS_FAULT", mode, 1) == 0);
    const iris_device_selector_t selector = {cti, NULL, NULL, "test-serial"};
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &camera.device));
    OK(iris_stream_open(camera.device, 0, &camera.stream));
    return camera;
}

static void expect_frame(const iris_frame_t *frame, const iris_buffer_t *buffer)
{
    EXPECT(frame->status == IRIS_FRAME_COMPLETE && frame->image_size == buffer->size);
    EXPECT(frame->data == buffer->data && frame->user_data == buffer->user_data);
    static const unsigned char pixels[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    EXPECT(memcmp(frame->data, pixels, sizeof pixels) == 0);
}

int main(int argc, char **argv)
{
    EXPECT(argc == 3);
    iris_context_t *context = NULL;
    allocation_begin();
    OK(iris_context_create(&context));
    const allocation_counts calibration = allocation_end();
    EXPECT(!IRIS_TEST_ALLOCATIONS || calibration.consumer_calls > 0);
    camera_t first = open_camera(context, argv[1], "progressive-complete");
    camera_t second = open_camera(context, argv[2], "progressive");

    _Alignas(64) unsigned char a[16] = {0}, b[16] = {0};
    const iris_buffer_t buffer_a = {a, sizeof a, a}, buffer_b = {b, sizeof b, b};
    const iris_start_options_t start_a = {1, IRIS_ACQUISITION_STREAM_ONLY, &buffer_a};
    const iris_start_options_t start_b = {1, IRIS_ACQUISITION_STREAM_ONLY, &buffer_b};
    iris_frame_t frame_a, frame_b;

    OK(iris_stream_start(second.stream, &start_b));
    for (unsigned cycle = 0; cycle < 20; ++cycle) {
        OK(iris_stream_start(first.stream, &start_a));
        for (unsigned frame = 0; frame < 5; ++frame) {
            memset(a, 0, sizeof a);
            memset(b, 0, sizeof b);
            allocation_begin();
            EXPECT(iris_stream_try_get_frame(second.stream, &frame_b) == IRIS_NO_FRAME);
            OK(iris_stream_try_get_frame(first.stream, &frame_a));
            expect_frame(&frame_a, &buffer_a);
            EXPECT(iris_stream_try_get_frame(second.stream, &frame_b) == IRIS_NO_FRAME);
            OK(iris_stream_try_get_frame(second.stream, &frame_b));
            expect_frame(&frame_b, &buffer_b);
            expect_frame(&frame_a, &buffer_a);
            OK(iris_stream_release_frame(second.stream, &frame_b));
            OK(iris_stream_release_frame(first.stream, &frame_a));
            const allocation_counts counts = allocation_end();
            EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
        }
        OK(iris_stream_stop(first.stream));
        EXPECT(iris_stream_try_get_frame(first.stream, &frame_a) == IRIS_STOPPED);
    }
    OK(iris_stream_close(&first.stream));
    OK(iris_device_close(&first.device));
    allocation_begin();
    EXPECT(iris_stream_try_get_frame(second.stream, &frame_b) == IRIS_NO_FRAME);
    EXPECT(iris_stream_try_get_frame(second.stream, &frame_b) == IRIS_NO_FRAME);
    OK(iris_stream_try_get_frame(second.stream, &frame_b));
    expect_frame(&frame_b, &buffer_b);
    OK(iris_stream_release_frame(second.stream, &frame_b));
    const allocation_counts counts = allocation_end();
    EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
    OK(iris_stream_stop(second.stream));
    OK(iris_stream_close(&second.stream));
    OK(iris_device_close(&second.device));
    OK(iris_context_close(&context));
    printf("Both in-tree CTIs streamed across 20 restart cycles%s.\n",
           IRIS_TEST_ALLOCATIONS ? " with zero Iris allocations" : "");
    return 0;
}
