#include <iris.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

bool check(iris_status_t status, const char *call)
{
    if (status == IRIS_OK) return true;
    iris_error_t error{};
    if (status < 0) iris_last_error(&error);
    std::fprintf(stderr, "%s returned %d: %s\n", call, status, error.message ? error.message : "");
    return false;
}

#define CHECK(call) check((call), #call)

bool configure(iris_node_map_t *remote, double exposure)
{
    iris_feature_info_t info{};
    if (!CHECK(iris_feature_info(remote, "ExposureTime", &info))) return false;
    if (info.type != IRIS_FEATURE_FLOAT || info.access != IRIS_FEATURE_READ_WRITE) {
        std::fputs("ExposureTime must be a readable and writable Float\n", stderr);
        return false;
    }
    iris_float_range_t range{};
    char unit[32]{};
    size_t size = sizeof(unit);
    if (!CHECK(iris_feature_float_range(remote, "ExposureTime", &range)) ||
        !CHECK(iris_feature_unit(remote, "ExposureTime", unit, &size))) return false;
    std::printf("ExposureTime range %g to %g %s\n", range.minimum, range.maximum, unit);
    if (!CHECK(iris_feature_set_float(remote, "ExposureTime", exposure))) return false;
    double observed = 0;
    if (!CHECK(iris_feature_get_float(remote, "ExposureTime", &observed))) return false;
    std::printf("ExposureTime = %g %s\n", observed, unit);

    if (!CHECK(iris_feature_set_enumeration(remote, "AcquisitionMode", "Continuous"))) return false;
    char mode[32]{};
    size = sizeof(mode);
    if (!CHECK(iris_feature_get_enumeration(remote, "AcquisitionMode", mode, &size))) return false;
    std::printf("AcquisitionMode = %s\n", mode);
    return true;
}

} // namespace

extern "C" void count_complete(void *client_data, const iris_frame_t *frame)
{
    if (frame->status == IRIS_FRAME_COMPLETE) {
        // Process frame->data here, while the image storage is borrowed.
        ++*static_cast<size_t *>(client_data);
    }
}

namespace {

bool acquire(iris_stream_t *stream)
{
    using namespace std::chrono_literals;
    size_t complete = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (complete < 8 && std::chrono::steady_clock::now() < deadline) {
        size_t delivered = 0;
        const auto status = iris_stream_poll(stream, count_complete, &complete, 4, &delivered);
        if (status != IRIS_BUSY && !check(status, "iris_stream_poll")) return false;
        if (!delivered) std::this_thread::sleep_for(1ms);
    }
    iris_stream_stats_t stats{};
    if (!CHECK(iris_stream_stats(stream, &stats))) return false;
    std::printf("Complete frames %zu of %llu completions\n", complete,
                static_cast<unsigned long long>(stats.delivered_frames));
    return complete >= 8;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s CTI SERIAL EXPOSURE_IN_FEATURE_UNITS\n", argv[0]);
        return EXIT_FAILURE;
    }
    char *end = nullptr;
    errno = 0;
    const double exposure = std::strtod(argv[3], &end);
    if (errno || end == argv[3] || *end || !std::isfinite(exposure)) {
        std::fputs("Expected a finite numeric exposure\n", stderr);
        return EXIT_FAILURE;
    }

    iris_context_t *context = nullptr;
    iris_device_t *device = nullptr;
    iris_node_map_t *remote = nullptr;
    iris_stream_t *stream = nullptr;
    const iris_device_selector_t selector{argv[1], nullptr, nullptr, argv[2]};
    bool ok = CHECK(iris_context_create(&context)) &&
              CHECK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 1000, &device)) &&
              CHECK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote)) &&
              configure(remote, exposure) &&
              CHECK(iris_stream_open(device, 0, &stream)) &&
              CHECK(iris_stream_start(stream, nullptr)) &&
              acquire(stream);
    // Stop also retries cleanup if stream startup failed partway through.
    if (stream) {
        if (!CHECK(iris_stream_stop(stream))) ok = false;
        if (!CHECK(iris_stream_close(&stream))) ok = false;
    }
    if (device && !CHECK(iris_device_close(&device))) ok = false;
    if (context && !CHECK(iris_context_close(&context))) ok = false;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
