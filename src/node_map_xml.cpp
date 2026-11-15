#include "node_map.hpp"

#include "gentl_info.hpp"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <strings.h>

using namespace GenTL;
using namespace iris;

namespace {

namespace fs = std::filesystem;
constexpr auto url_operation = "GenApi XML URL";
constexpr auto override_operation = "GenApi XML override";
constexpr uint32_t max_urls = 1024;

uint64_t hex(std::string_view text)
{
    if (text.substr(0, 2) == "0x" || text.substr(0, 2) == "0X") text.remove_prefix(2);
    uint64_t value = 0;
    const auto end = text.data() + text.size();
    const auto result = std::from_chars(text.data(), end, value, 16);
    require(result.ec == std::errc{} && result.ptr == end,
            IRIS_PRODUCER_ERROR, url_operation, "invalid hexadecimal field");
    return value;
}

bool starts_with(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() &&
        strncasecmp(text.data(), prefix.data(), prefix.size()) == 0;
}

fs::path xml_file(std::string_view path, const Producer &producer)
{
    path = path.substr(0, path.find_first_of("?#"));
    if (path.substr(0, 2) == "//") {
        path.remove_prefix(2);
        if (starts_with(path, "localhost/")) path.remove_prefix(9);
        require(!path.empty() && path.front() == '/', IRIS_NOT_SUPPORTED,
                url_operation, "file URL has a nonlocal host");
    }
    std::string decoded;
    for (size_t i = 0; i < path.size(); ++i) {
        char byte = path[i];
        if (byte == '%') {
            require(path.size() - i >= 3, IRIS_PRODUCER_ERROR, url_operation, "incomplete URL escape");
            unsigned value = 0;
            const auto end = path.data() + i + 3;
            const auto result = std::from_chars(path.data() + i + 1, end, value, 16);
            require(result.ec == std::errc{} && result.ptr == end && value,
                    IRIS_PRODUCER_ERROR, url_operation, "invalid URL escape");
            byte = static_cast<char>(value);
            i += 2;
        }
        decoded.push_back(byte);
    }
    require(!decoded.empty(), IRIS_PRODUCER_ERROR, url_operation, "empty file path");
    fs::path file(decoded);
    if (file.is_relative()) file = fs::path(producer.path).parent_path() / file;
    return file;
}

std::string file_url(const fs::path &file)
{
    constexpr std::string_view unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/";
    constexpr char digits[] = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char byte : file.native()) {
        if (unreserved.find(byte) != unreserved.npos) {
            url.push_back(static_cast<char>(byte));
        } else {
            url.push_back('%');
            url.push_back(digits[byte >> 4]);
            url.push_back(digits[byte & 15]);
        }
    }
    return url;
}

void read_xml_file(iris_node_map &map, const fs::path &file, const char *operation)
{
    std::error_code ec;
    require(fs::is_regular_file(file, ec), IRIS_IO_ERROR, operation, "XML file is unavailable");
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    const std::streamoff length = input.tellg();
    require(length >= 0, IRIS_IO_ERROR, operation, "cannot open XML file");
    require(length > 0 && length <= static_cast<std::streamoff>(max_node_map_xml_bytes),
            IRIS_OUT_OF_RANGE, operation, "invalid XML file size");
    map.xml_source.resize(static_cast<size_t>(length));
    require(input.seekg(0) && input.read(map.xml_source.data(), length), IRIS_IO_ERROR,
            operation, "cannot read XML file");
}

void read_xml_url(iris_node_map &map, std::string_view url)
{
    if (starts_with(url, "local:")) {
        const auto a = url.find(';');
        const auto b = a == url.npos ? url.npos : url.find(';', a + 1);
        require(b != url.npos, IRIS_PRODUCER_ERROR, url_operation, "invalid local URL");
        const auto address = hex(url.substr(a + 1, b - a - 1));
        auto extent = url.substr(b + 1);
        const auto length = hex(extent.substr(0, extent.find_first_of(";?#")));
        require(length && length <= max_node_map_xml_bytes && address <= INT64_MAX - length,
                IRIS_OUT_OF_RANGE, url_operation, "invalid XML extent");
        map.xml_source.resize(length);
        map.port.Read(map.xml_source.data(), address, length);
    } else if (starts_with(url, "file:")) {
        read_xml_file(map, xml_file(url.substr(5), map.port.producer), url_operation);
    } else {
        throw Failure{fail(IRIS_NOT_SUPPORTED, url_operation, "unsupported XML URL scheme")};
    }
}

void connect_xml(iris_node_map &map, const std::string &port_name, GenApi::ECacheUsage_t cache)
{
    auto &source = map.xml_source;
    map.xml_zipped = source.compare(0, 4, "PK\x03\x04") == 0;
    // Some register-backed XML descriptions include a terminating NUL.
    if (!map.xml_zipped && source.find('\0') == source.size() - 1) source.pop_back();
    const auto content = map.xml_zipped ? GenApi::ContentType_ZippedXml : GenApi::ContentType_Xml;
    GenApi::CNodeMapFactory factory(content, source.data(), source.size(), cache);
    const auto destroy = [](GenApi::INodeMap *nodes) { GenApi::CastToIDestroy(nodes)->Destroy(); };
    std::unique_ptr<GenApi::INodeMap, decltype(destroy)> nodes(factory.CreateNodeMap(), destroy);
    require(nodes.get(), IRIS_GENAPI_ERROR, "GenApi XML load", "missing node map");
    map.nodes = nodes.get();
    nodes.release();
    require(port_name.empty() ? map.nodes._Connect(&map.port)
                              : map.nodes._Connect(&map.port, port_name.c_str()),
            IRIS_GENAPI_ERROR, "GenApi Connect", "node map port connection failed");
}

template<class Query>
std::string url_bytes(Producer &p, const char *operation, Query query)
{
    size_t size = 0;
    const auto result = query(nullptr, &size);
    if (result != GC_ERR_BUFFER_TOO_SMALL) p.check(result, operation);
    require(size && size <= 1024 * 1024, IRIS_PRODUCER_ERROR, operation, "invalid URL data size");
    std::string bytes(size, '\xff');
    p.check(query(bytes.data(), &size), operation);
    require(size && size <= bytes.size() && bytes[size - 1] == '\0',
            IRIS_PRODUCER_ERROR, operation, "invalid URL data");
    bytes.resize(size);
    return bytes;
}

void xml_versions(Producer &p, PORT_HANDLE port, iris_node_map_xml_info_t &info)
{
    const auto get = [&](URL_INFO_CMD field, int32_t &value) {
        bool available = false;
        api("GCGetPortURLInfo", [&] {
            available = get_info(value, [&](auto *type, auto *data, auto *size) {
                return p.GCGetPortURLInfo(port, info.url_index, field, type, data, size);
            });
            return IRIS_OK;
        });
        return available && value >= 0;
    };
    if (get(URL_INFO_SCHEMA_VER_MAJOR, info.schema_major) &&
        get(URL_INFO_SCHEMA_VER_MINOR, info.schema_minor)) info.valid |= IRIS_XML_HAS_SCHEMA_VERSION;
    if (get(URL_INFO_FILE_VER_MAJOR, info.file_major) &&
        get(URL_INFO_FILE_VER_MINOR, info.file_minor) &&
        get(URL_INFO_FILE_VER_SUBMINOR, info.file_subminor)) info.valid |= IRIS_XML_HAS_FILE_VERSION;
}

} // namespace

std::unique_ptr<iris_node_map> iris::load_node_map(Producer &p, PORT_HANDLE port,
                                                 bool writable, std::mutex &mutex, const char *xml_path)
{
    std::lock_guard lock(mutex);
    const auto port_name = text_query(p, "GCGetPortInfo", [&](char *s, size_t *n) {
        INFO_DATATYPE type = 0;
        return p.GCGetPortInfo(port, PORT_INFO_PORTNAME, &type, s, n);
    }, true);

    if (xml_path) {
        std::error_code ec;
        const auto file = fs::absolute(xml_path, ec);
        require(!ec, IRIS_IO_ERROR, override_operation, "cannot resolve XML path");
        auto map = std::make_unique<iris_node_map>(p, port, writable, mutex);
        read_xml_file(*map, file, override_operation);
        if (map->xml_source.compare(0, 4, "PK\x03\x04") == 0)
            map->xml_source = unzip_xml(map->xml_source, override_operation);
        connect_xml(*map, port_name, GenApi::CacheUsage_Ignore);
        map->xml_url = file_url(file);
        map->xml_info.url = map->xml_url.c_str();
        map->xml_info.url_index = UINT32_MAX;
        map->xml_info.valid = IRIS_XML_IS_OVERRIDE;
        return map;
    }

    Error diagnostic;
    iris_status_t failure = IRIS_OK;
    const auto remember_failure = [&](iris_status_t status) {
        if (status == IRIS_NO_MEMORY) throw Failure{status};
        if (status < 0 && (failure == IRIS_OK ||
            (failure == IRIS_NOT_SUPPORTED && status != IRIS_NOT_SUPPORTED))) {
            diagnostic = last;
            failure = status;
        }
    };
    std::unique_ptr<iris_node_map> loaded;
    const auto try_url = [&](std::string_view url, uint32_t index, bool indexed) {
        auto map = std::make_unique<iris_node_map>(p, port, writable, mutex);
        read_xml_url(*map, url);
        connect_xml(*map, port_name, GenApi::CacheUsage_Automatic);
        map->xml_url = url;
        auto &info = map->xml_info;
        info.url = map->xml_url.c_str();
        info.url_index = index;
        info.indexed = indexed;
        if (indexed) xml_versions(p, port, info);
        loaded = std::move(map);
    };

    if (p.GCGetPortURLInfo) {
        uint32_t count = 1; // Some older CTIs export URLInfo without the count function.
        if (p.GCGetNumPortURLs) {
            const auto status = api("GCGetNumPortURLs", [&] {
                uint32_t reported = 0;
                p.check(p.GCGetNumPortURLs(port, &reported), "GCGetNumPortURLs");
                require(reported <= max_urls, IRIS_OUT_OF_RANGE, "GCGetNumPortURLs", "too many XML URLs");
                count = reported;
                return IRIS_OK;
            });
            if (status != IRIS_NOT_SUPPORTED && status < 0) count = 0;
            remember_failure(status);
        }
        for (uint32_t index = 0; index < count; ++index) {
            remember_failure(api("GCGetPortURLInfo", [&] {
                const auto bytes = url_bytes(p, "GCGetPortURLInfo", [&](char *data, size_t *size) {
                    INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
                    const auto status = p.GCGetPortURLInfo(port, index, URL_INFO_URL, &type, data, size);
                    if (data && status == GC_ERR_SUCCESS)
                        require(type == INFO_DATATYPE_STRING, IRIS_PRODUCER_ERROR,
                                "GCGetPortURLInfo", "XML URL is not a string");
                    return status;
                });
                try_url(bytes.c_str(), index, true);
                return IRIS_OK;
            }));
            if (loaded) return loaded;
        }
    }

    std::string legacy;
    remember_failure(api("GCGetPortURL", [&] {
        legacy = url_bytes(p, "GCGetPortURL", [&](char *data, size_t *size) {
            return p.GCGetPortURL(port, data, size);
        });
        return IRIS_OK;
    }));
    std::string_view remaining = legacy;
    for (uint32_t index = 0; !remaining.empty() && remaining.front() && index < max_urls; ++index) {
        const auto length = remaining.find('\0');
        const auto url = remaining.substr(0, length);
        remember_failure(api(url_operation, [&] {
            try_url(url, index, false);
            return IRIS_OK;
        }));
        if (loaded) return loaded;
        remaining.remove_prefix(length + 1);
    }
    if (failure == IRIS_OK)
        failure = fail(IRIS_NOT_SUPPORTED, url_operation, "no usable XML URL");
    else
        last = diagnostic;
    throw Failure{failure};
}

iris_status_t iris_node_map_xml_info(const iris_node_map_t *map, iris_node_map_xml_info_t *info)
{
    if (!map || !info) return invalid("node map XML info");
    *info = map->xml_info;
    return IRIS_OK;
}
