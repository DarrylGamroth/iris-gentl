#include <iris.h>

#include <stdio.h>

#define OK(call) do { \
    iris_status_t status = (call); \
    if (status != IRIS_OK) { \
        iris_error_t error; \
        iris_last_error(&error); \
        fprintf(stderr, "%s returned %d: %s\n", #call, status, error.message ? error.message : ""); \
        return 1; \
    } \
} while (0)

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    iris_context_t *context = NULL;
    iris_device_t *device = NULL;
    iris_node_map_t *remote = NULL;
    iris_stream_t *stream = NULL;
    const iris_device_selector_t selector = {argv[1], NULL, NULL, "test-serial"};
    const iris_start_options_t options = {1, IRIS_ACQUISITION_STREAM_ONLY, NULL};
    unsigned char image[16] = {0};
    const iris_row_target_t target = {image, sizeof image, 0x01100007, 2, 4};
    iris_row_update_t update;
    int64_t width;

    OK(iris_context_create(&context));
    OK(iris_device_open(context, &selector, IRIS_DEVICE_CONTROL, 0, &device));
    OK(iris_device_node_map(device, IRIS_REMOTE_DEVICE, &remote));
    OK(iris_feature_get_integer(remote, "Width", &width));
    if (width != 2) return 1;
    OK(iris_stream_open(device, 0, &stream));
    OK(iris_stream_start(stream, &options));
    OK(iris_stream_poll_rows(stream, &target, &update));
    if (update.first_row || update.row_count != 4 || !update.complete) return 1;
    for (size_t i = 0; i < sizeof image; ++i)
        if (image[i] != i + 1) return 1;
    OK(iris_stream_stop(stream));
    OK(iris_stream_close(&stream));
    OK(iris_device_close(&device));
    OK(iris_context_close(&context));
    puts("Installed Iris read GenApi Width and captured a complete mock frame");
    return 0;
}
