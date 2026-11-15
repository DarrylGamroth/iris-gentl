#include "stream.hpp"
#include "node_map.hpp"

#include <cstdio>
#include <cstdlib>
#include <thread>

#define EXPECT(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static void unexpected_frame(void *, const iris_frame_t *) { EXPECT(false); }

static void poll_handler(void *opaque, const iris_frame_t *frame)
{
    auto *stream = static_cast<iris_stream_t *>(opaque);
    EXPECT(frame->status == IRIS_FRAME_COMPLETE);
    std::thread contender([&] {
        // The admission lock stays held after the producer delivers a buffer
        // and through the callback. A competing reader returns BUSY.
        const bool acquired = stream->mutex.try_lock();
        if (acquired) stream->mutex.unlock();
        EXPECT(!acquired);
        iris_frame_t other{};
        EXPECT(iris_stream_try_get_frame(stream, &other) == IRIS_BUSY);
        size_t delivered = 99;
        EXPECT(iris_stream_poll(stream, unexpected_frame, nullptr, 1, &delivered) == IRIS_BUSY);
        EXPECT(delivered == 0);
    });
    contender.join();
}

int main(int argc, char **argv)
{
    EXPECT(argc == 2);
    EXPECT(setenv("IRIS_FAULT", "siblings", 1) == 0);
    iris_context_t *context = nullptr;
    iris_device_t *device = nullptr;
    iris_stream_t *stream = nullptr;
    const iris_device_selector_t selector{argv[1], nullptr, nullptr, "test-serial"};
    const iris_start_options_t options{1, IRIS_ACQUISITION_STREAM_ONLY, nullptr};
    EXPECT(iris_context_create(&context) == IRIS_OK);
    EXPECT(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device) == IRIS_OK);
    EXPECT(iris_stream_open(device, 0, &stream) == IRIS_OK);
    iris_stream_t *sibling = nullptr;
    EXPECT(iris_stream_open(device, 1, &sibling) == IRIS_OK);
    EXPECT(reinterpret_cast<uintptr_t>(stream) % iris::cache_line_size == 0);
    EXPECT(reinterpret_cast<uintptr_t>(sibling) % iris::cache_line_size == 0);
    EXPECT(iris_stream_start(stream, &options) == IRIS_OK);
    const auto &slot = stream->buffers.front();
    EXPECT(reinterpret_cast<uintptr_t>(&slot) % iris::cache_line_size == 0);
    EXPECT(reinterpret_cast<uintptr_t>(slot.parts.data()) % iris::cache_line_size == 0);
    EXPECT(iris_stream_start(sibling, &options) == IRIS_BUSY);
    EXPECT(iris_device_close(&device) == IRIS_BUSY);
    size_t delivered = 0;
    EXPECT(iris_stream_poll(stream, poll_handler, stream, 1, &delivered) == IRIS_OK);
    EXPECT(delivered == 1);
    iris_frame_t frame{};
    EXPECT(iris_stream_try_get_frame(stream, &frame) == IRIS_OK);
    EXPECT(iris_stream_release_frame(stream, &frame) == IRIS_OK);
    EXPECT(iris_stream_stop(stream) == IRIS_OK);
    EXPECT(iris_stream_start(sibling, &options) == IRIS_OK);
    EXPECT(iris_stream_try_get_frame(sibling, &frame) == IRIS_OK);
    EXPECT(iris_stream_release_frame(sibling, &frame) == IRIS_OK);
    EXPECT(iris_stream_stop(sibling) == IRIS_OK);
    EXPECT(iris_stream_close(&sibling) == IRIS_OK);
    EXPECT(iris_stream_close(&stream) == IRIS_OK);
    EXPECT(iris_device_close(&device) == IRIS_OK);
    EXPECT(iris_context_close(&context) == IRIS_OK);
}
