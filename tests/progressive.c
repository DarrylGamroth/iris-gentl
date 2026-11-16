#define _POSIX_C_SOURCE 200809L
#include "iris.h"
#include "allocations.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) EXPECT((x) == IRIS_OK)

static void run(const char *path, const char *scenario)
{
    EXPECT(setenv("IRIS_FAULT", scenario, 1) == 0);
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_stream_t *stream = NULL;
    const iris_device_selector_t selector = {path, NULL, NULL, "test-serial"};
    const iris_start_options_t options = {1, IRIS_ACQUISITION_STREAM_ONLY, NULL};
    unsigned char image[16] = {0};
    iris_row_target_t target = {image, sizeof image, 0x01100007, 2, 4};
    iris_row_update_t update;

    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_stream_open(device, 0, &stream));
    OK(iris_stream_start(stream, &options));

    if (!strcmp(scenario, "timeout")) {
        EXPECT(iris_stream_poll_rows(stream, &target, &update) == IRIS_NOT_SUPPORTED);
    } else if (!strcmp(scenario, "progressive-complete")) {
        OK(iris_stream_poll_rows(stream, &target, &update));
        EXPECT(update.first_row == 0 && update.row_count == 4 && update.complete);
        EXPECT(image[15] == 16);
    } else {
        allocation_begin();
        OK(iris_stream_poll_rows(stream, &target, &update));
        EXPECT(update.first_row == 0 && update.row_count == 1 && !update.complete);
        EXPECT(image[0] == 1 && image[3] == 4 && !image[4]);
        if (!strcmp(scenario, "progressive-regress")) {
            EXPECT(iris_stream_poll_rows(stream, &target, &update) == IRIS_IO_ERROR);
            EXPECT(!update.row_count && !update.complete && !image[4]);
        } else {
            iris_row_target_t changed = target;
            changed.capacity++;
            EXPECT(iris_stream_poll_rows(stream, &changed, &update) == IRIS_INVALID_STATE);
            iris_frame_t frame;
            EXPECT(iris_stream_try_get_frame(stream, &frame) == IRIS_BUSY);
            OK(iris_stream_poll_rows(stream, &target, &update));
            EXPECT(update.first_row == 1 && update.row_count == 2 && !update.complete);
            EXPECT(image[11] == 12 && !image[12]);
            if (!strcmp(scenario, "progressive-incomplete")) {
                EXPECT(iris_stream_poll_rows(stream, &target, &update) == IRIS_IO_ERROR);
                EXPECT(!update.row_count && !update.complete && !image[12]);
            } else {
                OK(iris_stream_poll_rows(stream, &target, &update));
                EXPECT(update.first_row == 3 && update.row_count == 1 && update.complete);
                EXPECT(image[15] == 16);
            }
        }
        const allocation_counts counts = allocation_end();
        EXPECT(counts.consumer_calls == 0 && counts.consumer_bytes == 0);
    }

    OK(iris_stream_stop(stream));
    EXPECT(iris_stream_poll_rows(stream, &target, &update) == IRIS_STOPPED);
    OK(iris_stream_close(&stream));
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    run(argv[1], "progressive");
    run(argv[1], "progressive-complete");
    run(argv[1], "progressive-regress");
    run(argv[1], "progressive-incomplete");
    run(argv[1], "timeout");
    return 0;
}
