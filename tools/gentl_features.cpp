#include "gentl_features.hpp"
#include "gentl_output.hpp"

#include <algorithm>
#include <cinttypes>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
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

namespace {

constexpr char snapshot_format[] = "iris-gentl snapshot 1";
using Selectors = std::map<std::string, std::string>;

struct FeatureKey {
    std::string module, name;
    Selectors selectors;

    bool operator<(const FeatureKey &other) const
    {
        return std::tie(module, name, selectors) < std::tie(other.module, other.name, other.selectors);
    }
};

using Snapshot = std::map<FeatureKey, std::string>;

Selectors read_selectors(iris_node_map_t *map, const std::string &name)
{
    Selectors values;
    std::set<std::string> visited{name};
    auto pending = related(map, name.c_str(), IRIS_SELECTING_FEATURES);
    while (!pending.empty()) {
        auto selector = std::move(pending.back());
        pending.pop_back();
        if (!visited.insert(selector).second) continue;
        values.emplace(selector, read_feature(map, selector.c_str()));
        for (auto &parent : related(map, selector.c_str(), IRIS_SELECTING_FEATURES))
            pending.push_back(std::move(parent));
    }
    return values;
}

Json feature_key(const FeatureKey &key)
{
    Json entry, selectors;
    entry.string("module", key.module);
    entry.string("name", key.name);
    for (const auto &[selector, value] : key.selectors) selectors.string(selector, value);
    entry.raw("selectors", selectors.str());
    return entry;
}

std::string read_field(std::istream &input)
{
    std::string field;
    if (input.peek() != '"' || !(input >> std::quoted(field)))
        throw std::invalid_argument("invalid quoted field");
    return field;
}

void read_separator(std::istream &input, char expected)
{
    if (input.get() != expected) throw std::invalid_argument("invalid field separator");
}

Snapshot read_snapshot(const std::string &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::invalid_argument("cannot open snapshot " + path);
    std::string header;
    if (!std::getline(input, header) || header != snapshot_format)
        throw std::invalid_argument("invalid snapshot header in " + path);
    Snapshot values;
    try {
        while (input.peek() != std::char_traits<char>::eof()) {
            FeatureKey key;
            key.module = read_field(input);
            read_separator(input, '\t');
            key.name = read_field(input);
            read_separator(input, '\t');
            size_t selectors;
            if (!(input >> selectors)) throw std::invalid_argument("invalid selector count");
            for (size_t i = 0; i < selectors; ++i) {
                read_separator(input, '\t');
                const auto selector = read_field(input);
                read_separator(input, '\t');
                if (!key.selectors.emplace(selector, read_field(input)).second)
                    throw std::invalid_argument("duplicate selector");
            }
            read_separator(input, '\t');
            auto value = read_field(input);
            read_separator(input, '\n');
            if (!values.emplace(std::move(key), std::move(value)).second)
                throw std::invalid_argument("duplicate feature and selector state");
        }
        if (!input.eof() || input.bad()) throw std::invalid_argument("read failed");
    } catch (const std::invalid_argument &error) {
        throw std::invalid_argument(path + ": " + error.what());
    }
    return values;
}

void print_change(const FeatureKey &key, const std::string *before, const std::string *after, bool json)
{
    const char *change = !before ? "added" : !after ? "removed" : "changed";
    if (json) {
        auto entry = feature_key(key);
        entry.string("change", change);
        if (before) entry.string("before", *before);
        if (after) entry.string("after", *after);
        entry.print();
    } else {
        std::cout << change << ' ' << key.module << '/' << key.name;
        for (const auto &[selector, value] : key.selectors)
            std::cout << ' ' << selector << '=' << std::quoted(value);
        if (before) std::cout << " before=" << std::quoted(*before);
        if (after) std::cout << " after=" << std::quoted(*after);
        std::cout << '\n';
    }
}

} // namespace

void snapshot(iris_node_map_t *map, const std::string &module, const std::vector<std::string> &names, bool json)
{
    Snapshot values;
    for (const auto &name : names) {
        auto value = read_feature(map, name.c_str());
        values.emplace(FeatureKey{module, name, read_selectors(map, name)}, std::move(value));
    }
    if (!json) std::cout << snapshot_format << '\n';
    for (const auto &[key, value] : values) {
        if (json) {
            auto entry = feature_key(key);
            entry.string("value", value);
            entry.print();
        } else {
            std::cout << std::quoted(key.module) << '\t' << std::quoted(key.name) << '\t' << key.selectors.size();
            for (const auto &[selector, selected] : key.selectors)
                std::cout << '\t' << std::quoted(selector) << '\t' << std::quoted(selected);
            std::cout << '\t' << std::quoted(value) << '\n';
        }
    }
    if (!std::cout) throw std::runtime_error("could not write snapshot");
}

bool diff_snapshots(const std::string &before, const std::string &after, bool json)
{
    const auto old_values = read_snapshot(before);
    const auto new_values = read_snapshot(after);
    bool equal = true;
    auto old_entry = old_values.begin(), new_entry = new_values.begin();
    while (old_entry != old_values.end() || new_entry != new_values.end()) {
        if (old_entry == old_values.end() ||
            (new_entry != new_values.end() && new_entry->first < old_entry->first)) {
            print_change(new_entry->first, nullptr, &new_entry->second, json);
            ++new_entry;
        } else if (new_entry == new_values.end() || old_entry->first < new_entry->first) {
            print_change(old_entry->first, &old_entry->second, nullptr, json);
            ++old_entry;
        } else {
            if (old_entry->second != new_entry->second) {
                print_change(old_entry->first, &old_entry->second, &new_entry->second, json);
                equal = false;
            }
            ++old_entry;
            ++new_entry;
            continue;
        }
        equal = false;
    }
    return equal;
}

} // namespace gentl
