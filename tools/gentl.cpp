#include <iris.h>

#include <charconv>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr char usage[] =
    "Usage: iris-gentl COMMAND [OPTIONS] [ARGS]\n"
    "  info             List devices and their GenTL IDs\n"
    "  features [GLOB]  List GenApi features, types and access modes\n"
    "  get NAME ...     Read feature values\n"
    "  set NAME=VALUE ...  Write features in order\n"
    "  exec NAME        Execute a GenApi command\n"
    "  xml              Write the active node-map XML to stdout\n"
    "  event [COUNT]    Print GenTL events (default: 1)\n"
    "  grab [COUNT]     Receive complete images (default: 1)\n"
    "\n"
    "  --cti PATH       CTI path or filename. Repeat for info. With no CTI,\n"
    "                   info scans GENICAM_GENTL64_PATH. Other commands require one.\n"
    "  --serial SERIAL  Select by serial, optionally narrowed by --interface\n"
    "  --interface ID   Select interface; combine with --device for exact IDs\n"
    "  --device ID      Select by exact GenTL device ID within --interface\n"
    "  --module NAME    remote (default), device, interface, system or stream\n"
    "  --stream INDEX   Zero-based data stream (default: 0)\n"
    "  --set NAME=VALUE Set a feature before running the command. Repeat in order.\n"
    "  --timeout MS     Time limit per completion or discovery update (default: 1000)\n"
    "  --buffers N      Grab with N announced buffers\n"
    "  --stream-only    Grab without issuing remote start and stop\n"
    "  --trigger NAME   Execute a remote command before each grab\n"
    "  --output FILE    Save grabbed image bytes, including padding, in order\n"
    "  -h, --help       Show help\n";

bool report(iris_status_t status, const char *operation)
{
    if (status == IRIS_OK) return true;
    iris_error_t error{};
    if (status < 0) iris_last_error(&error);
    std::fprintf(stderr, "%s: Iris status %" PRId32 "%s%s\n", operation, status,
                 error.message ? ": " : "", error.message ? error.message : "");
    return false;
}

struct operation_failed {};

void check(iris_status_t status, const char *operation)
{
    if (!report(status, operation)) throw operation_failed{};
}

struct Camera {
    iris_context_t *context = nullptr;
    iris_device_t *device = nullptr;
    iris_stream_t *stream = nullptr;
    bool acquiring = false;

    bool close()
    {
        if (stream && acquiring) {
            if (!report(iris_stream_stop(stream), "stop stream")) return false;
            acquiring = false;
        }
        if (stream && !report(iris_stream_close(&stream), "close stream")) return false;
        if (device && !report(iris_device_close(&device), "close device")) return false;
        return !context || report(iris_context_close(&context), "close context");
    }

    ~Camera() { close(); }
};

struct Devices {
    iris_device_list_t *list = nullptr;
    ~Devices() { iris_device_list_close(&list); }
};

struct Features {
    iris_feature_list_t *list = nullptr;
    ~Features() { iris_feature_list_close(&list); }
};

struct Frame {
    iris_stream_t *stream;
    iris_frame_t info{};
    ~Frame() { if (info.token) report(iris_stream_release_frame(stream, &info), "release frame"); }
};

uint64_t number(std::string_view text, uint64_t min, uint64_t max, const char *name)
{
    uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        result < min || result > max)
        throw std::invalid_argument(std::string("invalid ") + name + ": " + std::string(text));
    return result;
}

struct Options {
    std::vector<std::string> producers, args, assignments;
    std::string serial, interface, device, module = "remote", output, trigger;
    uint64_t timeout = 1000;
    size_t stream = 0;
    uint32_t buffers = 0;
    bool stream_only = false;
};

Options parse(int argc, char **argv)
{
    Options options;
    bool positional = false;
    for (int i = 2; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (!positional && arg == "--") { positional = true; continue; }
        if (positional || arg.empty() || arg.front() != '-') {
            options.args.emplace_back(arg);
            continue;
        }
        if (arg == "--stream-only") { options.stream_only = true; continue; }
        const auto equals = arg.find('=');
        const auto flag = arg.substr(0, equals);
        const auto value = equals != arg.npos ? arg.substr(equals + 1)
            : i + 1 < argc ? std::string_view(argv[++i]) : std::string_view{};
        if (value.empty()) throw std::invalid_argument(std::string(flag) + " needs a value");
        if (flag == "--cti") {
            fs::path path(value);
            options.producers.emplace_back(path.has_parent_path() ? fs::absolute(path).string() : path.string());
        } else if (flag == "--serial") options.serial = value;
        else if (flag == "--interface") options.interface = value;
        else if (flag == "--device") options.device = value;
        else if (flag == "--module") options.module = value;
        else if (flag == "--output") options.output = value;
        else if (flag == "--trigger") options.trigger = value;
        else if (flag == "--set") options.assignments.emplace_back(value);
        else if (flag == "--timeout") options.timeout = number(value, 1, UINT32_MAX, "timeout");
        else if (flag == "--stream") options.stream = number(value, 0, SIZE_MAX, "stream index");
        else if (flag == "--buffers") options.buffers = number(value, 1, UINT32_MAX, "buffer count");
        else throw std::invalid_argument("unknown option: " + std::string(flag));
    }
    return options;
}

std::vector<std::string> installed_producers()
{
    const char *path = std::getenv("GENICAM_GENTL64_PATH");
    std::set<std::string> paths;
    std::string_view remaining = path ? path : "";
    while (!remaining.empty()) {
        const auto colon = remaining.find(':');
        const auto directory = remaining.substr(0, colon);
        if (!directory.empty()) {
            std::error_code error;
            fs::directory_iterator iter(fs::path(directory), error), end;
            if (error && error != std::errc::no_such_file_or_directory)
                throw fs::filesystem_error("scan GenTL path", fs::path(directory), error);
            for (; iter != end; ++iter) {
                if (iter->path().extension() == ".cti" && iter->is_regular_file())
                    paths.insert(fs::canonical(iter->path()).string());
            }
        }
        if (colon == remaining.npos) break;
        remaining.remove_prefix(colon + 1);
    }
    return {paths.begin(), paths.end()};
}

const char *text(const char *value)
{
    return value ? value : "-";
}

bool matches(const iris_device_info_t &device, const Options &options)
{
    return (options.interface.empty() || options.interface == text(device.interface_id)) &&
           (options.device.empty() || options.device == text(device.device_id)) &&
           (options.serial.empty() || (device.serial_number && options.serial == device.serial_number));
}

iris_device_info_t device_info(const Devices &devices, size_t index)
{
    iris_device_info_t info{};
    check(iris_device_list_get(devices.list, index, &info), "read discovery entry");
    return info;
}

bool list_devices(Camera &camera, const Options &options)
{
    const auto producers = options.producers.empty() ? installed_producers() : options.producers;
    if (producers.empty()) throw std::invalid_argument("supply --cti or set GENICAM_GENTL64_PATH");
    bool ok = true;
    std::puts("CTI\tINTERFACE\tDEVICE\tSERIAL\tVENDOR\tMODEL\tTRANSPORT");
    for (const auto &producer : producers) {
        Devices devices;
        if (!report(iris_discover(camera.context, producer.c_str(), options.timeout, &devices.list),
                    producer.c_str())) {
            ok = false;
            continue;
        }
        for (size_t i = 0; i < iris_device_list_count(devices.list); ++i) {
            const auto info = device_info(devices, i);
            if (matches(info, options))
                std::printf("%s\t%s\t%s\t%s\t%s\t%s\t%s\n", text(info.producer_path),
                            text(info.interface_id), text(info.device_id), text(info.serial_number),
                            text(info.vendor), text(info.model), text(info.tl_type));
        }
    }
    return ok;
}

void open_device(Camera &camera, const Options &options, iris_device_access_t access)
{
    Devices devices;
    check(iris_discover(camera.context, options.producers.front().c_str(), options.timeout,
                        &devices.list), "discover");
    iris_device_info_t selected{};
    for (size_t i = 0; i < iris_device_list_count(devices.list); ++i) {
        const auto info = device_info(devices, i);
        if (!matches(info, options)) continue;
        if (selected.device_id)
            throw std::runtime_error("multiple devices match; select a serial or exact interface and device IDs");
        selected = info;
    }
    if (!selected.device_id) throw std::runtime_error("no device matches; run info to list device IDs");
    const iris_device_selector_t selector{selected.producer_path, selected.interface_id,
                                          selected.device_id, nullptr};
    check(iris_device_open(camera.context, &selector, access, options.timeout, &camera.device),
          "open device");
}

iris_node_map_t *node_map(Camera &camera, const Options &options)
{
    iris_node_map_t *map = nullptr;
    if (options.module == "stream") {
        check(iris_stream_open(camera.device, options.stream, &camera.stream), "open stream");
        check(iris_stream_node_map(camera.stream, &map), "open stream node map");
    } else {
        iris_module_t module = IRIS_REMOTE_DEVICE;
        if (options.module == "device") module = IRIS_DEVICE_MODULE;
        else if (options.module == "interface") module = IRIS_INTERFACE_MODULE;
        else if (options.module == "system") module = IRIS_SYSTEM_MODULE;
        else if (options.module != "remote") throw std::invalid_argument("unknown module: " + options.module);
        check(iris_device_node_map(camera.device, module, &map), "open node map");
    }
    return map;
}

void validate_assignments(const std::vector<std::string> &assignments)
{
    for (const auto &assignment : assignments) {
        const auto equals = assignment.find('=');
        if (equals == assignment.npos || equals == 0)
            throw std::invalid_argument("expected NAME=VALUE: " + assignment);
    }
}

void write_features(iris_node_map_t *map, const std::vector<std::string> &assignments)
{
    for (const auto &assignment : assignments) {
        const auto equals = assignment.find('=');
        const auto name = assignment.substr(0, equals);
        check(iris_feature_from_string(map, name.c_str(), assignment.c_str() + equals + 1),
              name.c_str());
    }
}

void list_features(iris_node_map_t *map, const std::vector<std::string> &args)
{
    const iris_feature_filter_t filter{args.empty() ? nullptr : args.front().c_str(),
                                       IRIS_FEATURE_MATCH_GLOB, 0, 0};
    Features features;
    check(iris_node_map_list_features(map, &filter, &features.list), "list features");
    const char *types[] = {"-", "integer", "float", "boolean", "enumeration", "string",
                           "command", "other", "category", "register"};
    const char *access[] = {"-", "NI", "NA", "WO", "RO", "RW"};
    std::puts("NAME\tTYPE\tACCESS\tDISPLAY NAME");
    for (size_t i = 0; i < iris_feature_list_count(features.list); ++i) {
        iris_feature_entry_t entry{};
        check(iris_feature_list_get(features.list, i, &entry), "read feature entry");
        const auto type = static_cast<size_t>(entry.info.type);
        const auto mode = static_cast<size_t>(entry.info.access);
        std::printf("%s\t%s\t%s\t%s\n", entry.name, type < std::size(types) ? types[type] : "-",
                    mode < std::size(access) ? access[mode] : "-", text(entry.display_name));
    }
}

void read_features(iris_node_map_t *map, const std::vector<std::string> &names)
{
    for (const auto &name : names) {
        size_t size = 0;
        check(iris_feature_to_string(map, name.c_str(), nullptr, &size), name.c_str());
        std::vector<char> value(size);
        check(iris_feature_to_string(map, name.c_str(), value.data(), &size), name.c_str());
        std::printf("%s=%s\n", name.c_str(), value.data());
    }
}

void export_xml(iris_node_map_t *map)
{
    size_t size = 0;
    check(iris_node_map_xml(map, nullptr, &size), "size XML");
    std::vector<char> xml(size);
    check(iris_node_map_xml(map, xml.data(), &size), "read XML");
    if (std::fwrite(xml.data(), 1, size - 1, stdout) != size - 1)
        throw std::runtime_error("could not write XML");
}

void receive_events(iris_node_map_t *map, uint64_t count, uint64_t timeout)
{
    check(iris_node_map_enable_events(map, 1), "subscribe to events");
    for (uint64_t i = 0; i < count; ++i) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
        iris_node_map_event_t event{};
        auto status = iris_node_map_try_get_event(map, &event);
        while (status == IRIS_NO_EVENT && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
            status = iris_node_map_try_get_event(map, &event);
        }
        if (status == IRIS_NO_EVENT) throw std::runtime_error("timed out waiting for a GenTL event");
        check(status, "receive event");
        std::printf("event=%s bytes=%zu\n", event.id, event.data_size);
    }
    check(iris_node_map_enable_events(map, 0), "unsubscribe from events");
}

void grab(Camera &camera, iris_node_map_t *map, const Options &options, uint64_t count)
{
    check(iris_stream_open(camera.device, options.stream, &camera.stream), "open stream");
    const iris_start_options_t start{options.buffers, options.stream_only ? IRIS_ACQUISITION_STREAM_ONLY
                                                                         : IRIS_ACQUISITION_REMOTE_AND_STREAM,
                                    nullptr};
    camera.acquiring = true;
    check(iris_stream_start(camera.stream, &start), "start stream");
    const auto close_file = [](FILE *file) { std::fclose(file); };
    std::unique_ptr<FILE, decltype(close_file)> output(nullptr, close_file);
    if (!options.output.empty()) {
        output.reset(std::fopen(options.output.c_str(), "wbx"));
        if (!output) throw std::runtime_error("cannot create output file: " + options.output);
    }
    for (uint64_t i = 0; i < count; ++i) {
        if (!options.trigger.empty())
            check(iris_feature_execute(map, options.trigger.c_str()), options.trigger.c_str());
        Frame frame{camera.stream};
        check(iris_stream_wait_frame(camera.stream, options.timeout, &frame.info), "wait for frame");
        const auto &info = frame.info;
        if (info.status != IRIS_FRAME_COMPLETE)
            throw std::runtime_error("received a rejected image (frame status " +
                                     std::to_string(info.status) + ")");
        if (output && std::fwrite(info.data, 1, info.image_size, output.get()) != info.image_size)
            throw std::runtime_error("could not write image to " + options.output);
        std::printf("frame=%" PRIu64 " width=%" PRIu64 " height=%" PRIu64
                    " pfnc=0x%" PRIx64 " stride=%zu bytes=%zu\n",
                    i + 1, info.width, info.height, info.pixel_format, info.stride_bytes, info.image_size);
        check(iris_stream_release_frame(camera.stream, &frame.info), "release frame");
    }
    if (output && std::fclose(output.release()) != 0)
        throw std::runtime_error("could not close image output " + options.output);
}

bool run(std::string_view command, const Options &options)
{
    const auto &args = options.args;
    const bool grabbing = command == "grab";
    const bool events = command == "event";
    const bool info = command == "info";
    const bool set = command == "set";
    if (command == "features" || grabbing || events) {
        if (args.size() > 1) throw std::invalid_argument("too many command arguments");
    } else if (command == "get" || set) {
        if (args.empty()) throw std::invalid_argument("feature arguments are required");
    } else if (command == "exec") {
        if (args.size() != 1) throw std::invalid_argument("exec requires one command name");
    } else if (info || command == "xml") {
        if (!args.empty()) throw std::invalid_argument("unexpected command arguments");
    } else throw std::invalid_argument("unknown command: " + std::string(command));
    if (!info && options.producers.size() != 1)
        throw std::invalid_argument("this command requires exactly one --cti");
    if (!grabbing && (options.buffers || options.stream_only || !options.output.empty() || !options.trigger.empty()))
        throw std::invalid_argument("buffer, stream-only, output and trigger options require grab");
    if ((info && !options.assignments.empty()) || (grabbing && options.module != "remote"))
        throw std::invalid_argument("invalid command options; see --help");
    validate_assignments(options.assignments);
    if (set) validate_assignments(args);
    const auto count = (grabbing || events) && !args.empty()
        ? number(args.front(), 1, UINT32_MAX, "count") : 1;

    Camera camera;
    check(iris_context_create(&camera.context), "create context");
    if (info) return list_devices(camera, options) && camera.close();

    const bool control = set || grabbing || events || command == "exec" || !options.assignments.empty();
    open_device(camera, options, control ? IRIS_DEVICE_CONTROL : IRIS_DEVICE_READ_ONLY);
    iris_node_map_t *map = nullptr;
    if (!grabbing || !options.assignments.empty() || !options.trigger.empty())
        map = node_map(camera, options);
    write_features(map, options.assignments);

    if (command == "features") list_features(map, args);
    else if (command == "get") read_features(map, args);
    else if (set) write_features(map, args);
    else if (command == "exec") check(iris_feature_execute(map, args.front().c_str()), args.front().c_str());
    else if (command == "xml") export_xml(map);
    else if (events) receive_events(map, count, options.timeout);
    else grab(camera, map, options, count);
    return camera.close();
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fputs(usage, stderr);
        return 2;
    }
    for (int i = 1; i < argc && std::string_view(argv[i]) != "--"; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::fputs(usage, stdout);
            return 0;
        }
    }
    try {
        return run(argv[1], parse(argc, argv)) && std::fflush(stdout) == 0 ? 0 : 1;
    } catch (const operation_failed &) {
        return 1;
    } catch (const std::invalid_argument &error) {
        std::fprintf(stderr, "iris-gentl: %s\n", error.what());
        return 2;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "iris-gentl: %s\n", error.what());
        return 1;
    }
}
