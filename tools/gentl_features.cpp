#include "gentl_features.hpp"
#include "gentl_output.hpp"

#include <algorithm>
#include <cinttypes>
#include <utility>

namespace gentl {
namespace {

struct Features {
    iris_feature_list_t *list = nullptr;
    ~Features() { iris_feature_list_close(&list); }
};

using TextReader = iris_status_t (*)(iris_node_map_t *, const char *, char *, size_t *);

std::string read_text(iris_node_map_t *map, const char *name, TextReader read)
{
    size_t size = 0;
    check(read(map, name, nullptr, &size), name);
    std::vector<char> value(size);
    check(read(map, name, value.data(), &size), name);
    return std::string(value.data(), size - 1);
}

std::vector<std::string> related(iris_node_map_t *map, const char *name, iris_feature_relation_t relation)
{
    Features features;
    check(iris_node_map_browse(map, name, relation, &features.list), name);
    std::vector<std::string> names;
    for (size_t i = 0; i < iris_feature_list_count(features.list); ++i) {
        iris_feature_entry_t entry{};
        check(iris_feature_list_get(features.list, i, &entry), "read related feature");
        names.emplace_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

const char *increment_mode(iris_increment_mode_t mode)
{
    switch (mode) {
        case IRIS_INCREMENT_FIXED: return "fixed";
        case IRIS_INCREMENT_LIST: return "list";
        default: return "none";
    }
}

std::string format_number(int64_t value) { return std::to_string(value); }
std::string format_number(double value) { return json_number(value); }

template<class Value>
void describe_range(iris_node_map_t *map, const char *name, Json &object, bool json,
                    iris_status_t (*range)(iris_node_map_t *, const char *, Value *),
                    iris_status_t (*values)(iris_node_map_t *, const char *, decltype(Value::minimum) *, size_t *))
{
    Value bounds{};
    check(range(map, name, &bounds), "read range");
    const auto low = format_number(bounds.minimum);
    const auto high = format_number(bounds.maximum);
    const auto unit = read_text(map, name, iris_feature_unit);
    if (json) {
        object.raw("minimum", low);
        object.raw("maximum", high);
        object.string("unit", unit);
        object.string("increment_mode", increment_mode(bounds.increment_mode));
    } else {
        std::printf("Range: %s to %s%s%s\nIncrement mode: %s\n", low.c_str(), high.c_str(),
                    unit.empty() ? "" : " ", unit.c_str(), increment_mode(bounds.increment_mode));
    }
    if (bounds.increment_mode == IRIS_INCREMENT_FIXED) {
        const auto increment = format_number(bounds.increment);
        if (json) object.raw("increment", increment);
        else std::printf("Increment: %s\n", increment.c_str());
    } else if (bounds.increment_mode == IRIS_INCREMENT_LIST) {
        size_t count = 0;
        check(values(map, name, nullptr, &count), "read value count");
        std::vector<decltype(Value::minimum)> entries(count);
        check(values(map, name, entries.data(), &count), "read values");
        Json list(true);
        if (!json) std::puts("Values:");
        for (size_t i = 0; i < count; ++i) {
            const auto value = format_number(entries[i]);
            if (json) list.item(value);
            else std::printf("  %s\n", value.c_str());
        }
        if (json) object.raw("values", list.str());
    }
}

void describe_entries(iris_node_map_t *map, const char *name, Json &object, bool json)
{
    size_t count;
    check(iris_feature_enumeration_count(map, name, &count), "count enumeration entries");
    Json entries(true);
    if (!json) std::puts("Choices:");
    for (size_t i = 0; i < count; ++i) {
        size_t size = 0;
        iris_feature_access_t access;
        check(iris_feature_enumeration_entry(map, name, i, nullptr, &size, &access), "size enumeration entry");
        std::vector<char> symbol(size);
        check(iris_feature_enumeration_entry(map, name, i, symbol.data(), &size, &access), "read enumeration entry");
        if (json) {
            Json item;
            item.string("symbol", symbol.data());
            item.string("access", feature_access(access));
            entries.item(item.str());
        } else std::printf("  %s (%s)\n", symbol.data(), feature_access(access));
    }
    if (json) object.raw("choices", entries.str());
}

void describe_links(iris_node_map_t *map, const char *name, Json &object, bool json,
                    iris_feature_relation_t relation, const char *key, const char *label)
{
    const auto names = related(map, name, relation);
    if (json) {
        Json list(true);
        for (const auto &related_name : names) list.item(json_string(related_name));
        object.raw(key, list.str());
    } else if (!names.empty()) {
        std::printf("%s:\n", label);
        for (const auto &related_name : names) std::printf("  %s\n", related_name.c_str());
    }
}

} // namespace

std::string read_feature(iris_node_map_t *map, const char *name)
{
    return read_text(map, name, iris_feature_to_string);
}

void describe(iris_node_map_t *map, const std::string &name, bool json)
{
    iris_feature_metadata_t metadata{};
    size_t size = 0;
    check(iris_feature_metadata(map, name.c_str(), &metadata, nullptr, &size), "size feature metadata");
    std::vector<char> strings(size);
    check(iris_feature_metadata(map, name.c_str(), &metadata, strings.data(), &size), "read feature metadata");
    const auto &feature = metadata.feature;
    Json object;
    if (json) {
        object.string("name", feature.name);
        object.string("type", feature_type(feature.info.type));
        object.string("access", feature_access(feature.info.access));
        object.string("display_name", feature.display_name);
        object.string("description", feature.description);
    } else {
        std::printf("%s (%s, %s)\n", feature.name, feature_type(feature.info.type),
                    feature_access(feature.info.access));
        if (*feature.display_name) std::printf("Display name: %s\n", feature.display_name);
        if (*feature.description) std::puts(feature.description);
    }

    if (feature.info.access >= IRIS_FEATURE_WRITE_ONLY) {
        if (feature.info.type == IRIS_FEATURE_INTEGER)
            describe_range(map, name.c_str(), object, json, iris_feature_integer_range, iris_feature_integer_values);
        else if (feature.info.type == IRIS_FEATURE_FLOAT)
            describe_range(map, name.c_str(), object, json, iris_feature_float_range, iris_feature_float_values);
        else if (feature.info.type == IRIS_FEATURE_ENUMERATION)
            describe_entries(map, name.c_str(), object, json);
        else if (feature.info.type == IRIS_FEATURE_STRING) {
            size_t maximum;
            check(iris_feature_string_max_length(map, name.c_str(), &maximum), "read string length");
            if (json) object.number("maximum_length", maximum);
            else std::printf("Maximum length: %zu\n", maximum);
        }
    }
    describe_links(map, name.c_str(), object, json, IRIS_SELECTING_FEATURES, "selectors", "Selectors");
    describe_links(map, name.c_str(), object, json, IRIS_SELECTED_FEATURES, "selected", "Controlled features");
    if (json) object.print();
}

} // namespace gentl
