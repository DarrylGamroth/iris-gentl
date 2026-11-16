#define _POSIX_C_SOURCE 200809L
#include "allocations.h"
#include "iris.h"

#include <stdio.h>
#include <stdlib.h>

#define EXPECT(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
#define OK(x) EXPECT((x) == IRIS_OK)

enum { max_test_count = 10, image_size = 16 };

static void run(const char *cti, const char *minimum, unsigned expected,
                uint32_t count, int caller_owned)
{
    char expected_text[16];
    snprintf(expected_text, sizeof expected_text, "%u", expected);
    EXPECT(setenv("IRIS_MOCK_MIN_BUFFERS", minimum, 1) == 0);
    EXPECT(setenv("IRIS_MOCK_EXPECT_BUFFERS", expected_text, 1) == 0);

    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_stream_t *stream = NULL;
    const iris_device_selector_t selector = {cti, NULL, NULL, "test-serial"};
    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_stream_open(device, 0, &stream));

    _Alignas(64) unsigned char storage[max_test_count][image_size];
    iris_buffer_t buffers[max_test_count];
    for (unsigned i = 0; i < expected; ++i)
        buffers[i] = (iris_buffer_t){storage[i], sizeof storage[i], &storage[i]};
    const iris_start_options_t options = {
        count, IRIS_ACQUISITION_STREAM_ONLY, caller_owned ? buffers : NULL
    };
    if (count && !caller_owned) {
        iris_start_options_t wrong_count = options;
        --wrong_count.buffer_count;
        EXPECT(iris_stream_start(stream, &wrong_count) == IRIS_INVALID_ARGUMENT);
    }

    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        OK(iris_stream_start(stream, &options));
        iris_frame_t frames[max_test_count];
        const void *addresses[max_test_count];
        allocation_begin();
        for (unsigned i = 0; i < expected; ++i) {
            OK(iris_stream_try_get_frame(stream, &frames[i]));
            EXPECT(frames[i].status == IRIS_FRAME_COMPLETE && frames[i].image_size == image_size);
            addresses[i] = frames[i].data;
            if (caller_owned)
                EXPECT(addresses[i] == buffers[i].data && frames[i].user_data == buffers[i].user_data);
            for (unsigned j = 0; j < i; ++j) EXPECT(addresses[j] != addresses[i]);
        }
        iris_frame_t next;
        EXPECT(iris_stream_try_get_frame(stream, &next) == IRIS_NO_FRAME);
        for (unsigned i = expected; i > 0; --i) OK(iris_stream_release_frame(stream, &frames[i - 1]));
        for (unsigned i = expected; i > 0; --i) {
            OK(iris_stream_try_get_frame(stream, &next));
            EXPECT(next.status == IRIS_FRAME_COMPLETE && next.data == addresses[i - 1]);
            OK(iris_stream_release_frame(stream, &next));
        }
        const allocation_counts allocations = allocation_end();
        EXPECT(allocations.consumer_calls == 0 && allocations.consumer_bytes == 0);
        OK(iris_stream_stop(stream));
    }
    OK(iris_stream_close(&stream));
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    EXPECT(setenv("IRIS_FAULT", "progressive-pool", 1) == 0);
    run(argv[1], "1", 8, 0, 0);
    run(argv[1], "10", 10, 0, 0);
    run(argv[1], "2", 4, 4, 0);
    run(argv[1], "2", 4, 4, 1);
    return 0;
}
