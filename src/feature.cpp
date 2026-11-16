#include "node_map.hpp"

#include "error.hpp"
#include "feature.hpp"
#include "feature_values.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <regex>
#include <string>
#include <unordered_set>
#include <vector>

#include <fnmatch.h>
#include <strings.h>

using namespace iris;

namespace {
iris_feature_access_t feature_access(GenApi::EAccessMode access)
{
    switch (access) {
    case GenApi::NI: return IRIS_FEATURE_NOT_IMPLEMENTED;
    case GenApi::RO: return IRIS_FEATURE_READ_ONLY;
    case GenApi::WO: return IRIS_FEATURE_WRITE_ONLY;
    case GenApi::RW: return IRIS_FEATURE_READ_WRITE;
    default: return IRIS_FEATURE_NOT_AVAILABLE;
    }
}

iris_feature_info_t describe_feature(GenApi::INode &node)
{
    auto type = IRIS_FEATURE_OTHER;
    switch (node.GetPrincipalInterfaceType()) {
    case GenApi::intfIInteger: type = IRIS_FEATURE_INTEGER; break;
    case GenApi::intfIFloat: type = IRIS_FEATURE_FLOAT; break;
    case GenApi::intfIBoolean: type = IRIS_FEATURE_BOOLEAN; break;
    case GenApi::intfIEnumeration: type = IRIS_FEATURE_ENUMERATION; break;
    case GenApi::intfIString: type = IRIS_FEATURE_STRING; break;
    case GenApi::intfICommand: type = IRIS_FEATURE_COMMAND; break;
    case GenApi::intfICategory: type = IRIS_FEATURE_CATEGORY; break;
    case GenApi::intfIRegister: type = IRIS_FEATURE_REGISTER; break;
    default: break;
    }
    return {type, feature_access(node.GetAccessMode())};
}

iris_feature_visibility_t visibility(GenApi::EVisibility value)
{
    switch (value) {
    case GenApi::Beginner: return IRIS_VISIBILITY_BEGINNER;
    case GenApi::Expert: return IRIS_VISIBILITY_EXPERT;
    case GenApi::Guru: return IRIS_VISIBILITY_GURU;
    default: return IRIS_VISIBILITY_INVISIBLE;
    }
}

iris_feature_representation_t representation(GenApi::ERepresentation value)
{
    switch (value) {
    case GenApi::Linear: return IRIS_REPRESENTATION_LINEAR;
    case GenApi::Logarithmic: return IRIS_REPRESENTATION_LOGARITHMIC;
    case GenApi::Boolean: return IRIS_REPRESENTATION_BOOLEAN;
    case GenApi::PureNumber: return IRIS_REPRESENTATION_PURE_NUMBER;
    case GenApi::HexNumber: return IRIS_REPRESENTATION_HEX_NUMBER;
    case GenApi::IPV4Address: return IRIS_REPRESENTATION_IPV4_ADDRESS;
    case GenApi::MACAddress: return IRIS_REPRESENTATION_MAC_ADDRESS;
    default: return IRIS_REPRESENTATION_UNSPECIFIED;
    }
}

iris_feature_display_notation_t display_notation(GenApi::EDisplayNotation value)
{
    switch (value) {
    case GenApi::fnAutomatic: return IRIS_DISPLAY_NOTATION_AUTOMATIC;
    case GenApi::fnFixed: return IRIS_DISPLAY_NOTATION_FIXED;
    case GenApi::fnScientific: return IRIS_DISPLAY_NOTATION_SCIENTIFIC;
    default: return IRIS_DISPLAY_NOTATION_UNSPECIFIED;
    }
}

template<class T>
T &feature_value(iris_node_map_t &map, const char *name, const char *operation)
{
    auto *node = map.nodes._GetNode(name);
    require(node, IRIS_NOT_FOUND, operation, name);
    auto *value = dynamic_cast<T *>(node);
    require(value, IRIS_WRONG_TYPE, operation, name);
    return *value;
}

template<class T, class F>
iris_status_t feature(iris_node_map_t *map, const char *name, const char *operation, F f)
{
    if (!map || !name || !*name) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        return f(feature_value<T>(*map, name, operation));
    });
}

template<class T> iris_increment_mode_t increment_mode(T &v)
{
    switch (v.GetIncMode()) {
    case GenApi::fixedIncrement: return IRIS_INCREMENT_FIXED;
    case GenApi::listIncrement: return IRIS_INCREMENT_LIST;
    default: return IRIS_INCREMENT_NONE;
    }
}

int64_t register_length(GenApi::IRegister &reg, const char *operation)
{
    const auto length = reg.GetLength(true);
    require(length > 0, IRIS_OUT_OF_RANGE, operation, "invalid register length");
    return length;
}
}

FeatureEntry iris::describe_entry(GenApi::INode &node)
{
    return {node.GetName().c_str(), node.GetDisplayName().c_str(),
            node.GetDescription().c_str(), node.GetToolTip().c_str(),
            describe_feature(node), visibility(node.GetVisibility())};
}

FeatureEventID iris::event_id_with_dependencies(GenApi::INode &node)
{
    GenApi::NodeList_t pending;
    pending.push_back(&node);
    std::unordered_set<GenApi::INode *> visited;
    FeatureEventID event;
    while (!pending.empty()) {
        auto *current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) continue;
        const auto id = current->GetEventID();
        if (!id.empty()) {
            if (event.id.empty()) event.id = id.c_str();
            else if (strcasecmp(event.id.c_str(), id.c_str()) != 0) {
                event.ambiguous = true;
                break;
            }
        }
        GenApi::NodeList_t children;
        current->GetChildren(children);
        for (auto *child : children) pending.push_back(child);
    }
    return event;
}

iris_status_t iris::copy_text(const char *text, char *out, size_t *size)
{
    if (!size || (!out && *size)) return invalid("feature text");
    const size_t capacity = *size;
    *size = std::strlen(text) + 1;
    if (!out) return IRIS_OK;
    if (capacity < *size) return fail(IRIS_BUFFER_TOO_SMALL, "feature text", "output buffer too small");
    std::memcpy(out, text, *size);
    return IRIS_OK;
}

iris_status_t iris::read_register(GenApi::IRegister &reg, void *data, size_t *size, const char *operation)
{
    const auto length = register_length(reg, operation);
    const size_t capacity = *size;
    *size = static_cast<size_t>(length);
    if (!data) return IRIS_OK;
    if (capacity < *size) return fail(IRIS_BUFFER_TOO_SMALL, operation, "output buffer too small");
    reg.Get(static_cast<uint8_t *>(data), length, true, true);
    return IRIS_OK;
}

iris_status_t iris_feature_metadata(iris_node_map_t *map, const char *name,
                                    iris_feature_metadata_t *out, char *strings, size_t *size)
{
    constexpr auto operation = "feature metadata";
    if (!out || !size || (!strings && *size)) return invalid(operation);
    return feature<GenApi::INode>(map, name, operation, [&](auto &node) -> iris_status_t {
        const auto entry = describe_entry(node);
        const std::array fields{&entry.name, &entry.display_name, &entry.description, &entry.tooltip};
        size_t required = 0;
        for (const auto *field : fields) required += field->size() + 1;
        const auto capacity = *size;
        *size = required;
        if (strings && capacity < required)
            return fail(IRIS_BUFFER_TOO_SMALL, operation, "metadata string buffer too small");

        iris_feature_metadata_t metadata{};
        metadata.feature.info = entry.info;
        metadata.feature.visibility = entry.visibility;
        metadata.display_precision = -1;
        if (auto *floating = dynamic_cast<GenApi::IFloat *>(&node)) {
            metadata.representation = representation(floating->GetRepresentation());
            metadata.display_notation = display_notation(floating->GetDisplayNotation());
            const auto precision = floating->GetDisplayPrecision();
            if (precision >= 0) metadata.display_precision = precision;
        } else if (auto *integer = dynamic_cast<GenApi::IInteger *>(&node)) {
            metadata.representation = representation(integer->GetRepresentation());
        }
        if (strings) {
            const std::array destinations{&metadata.feature.name, &metadata.feature.display_name,
                                          &metadata.feature.description, &metadata.feature.tooltip};
            for (size_t i = 0; i < fields.size(); ++i) {
                const auto length = fields[i]->size() + 1;
                std::memcpy(strings, fields[i]->c_str(), length);
                *destinations[i] = strings;
                strings += length;
            }
        }
        *out = metadata;
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_browse(iris_node_map_t *map, const char *name,
                                  iris_feature_relation_t relation, iris_feature_list_t **out)
{
    if (!out) return invalid("browse features");
    *out = nullptr;
    if (relation < IRIS_CATEGORY_FEATURES || relation > IRIS_SELECTING_FEATURES)
        return invalid("browse features");
    if (!name && relation == IRIS_CATEGORY_FEATURES) name = "Root";
    return feature<GenApi::INode>(map, name, "browse features", [&](auto &node) {
        GenApi::FeatureList_t features;
        if (relation == IRIS_CATEGORY_FEATURES) {
            auto *category = dynamic_cast<GenApi::ICategory *>(&node);
            require(category, IRIS_WRONG_TYPE, "browse features", name);
            category->GetFeatures(features);
        } else if (auto *selector = dynamic_cast<GenApi::ISelector *>(&node)) {
            if (relation == IRIS_SELECTED_FEATURES) selector->GetSelectedFeatures(features);
            else selector->GetSelectingFeatures(features);
        }
        auto list = std::make_unique<iris_feature_list>();
        list->entries.reserve(features.size());
        for (auto *item : features) list->entries.push_back(describe_entry(*item->GetNode()));
        *out = list.release();
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_list_features(iris_node_map_t *map, const iris_feature_filter_t *filter,
                                          iris_feature_list_t **out)
{
    constexpr auto operation = "list features";
    if (!out) return invalid(operation);
    *out = nullptr;
    if (!map) return invalid(operation);
    const iris_feature_filter_t options = filter ? *filter : iris_feature_filter_t{};
    if (options.type < 0 || options.type > IRIS_FEATURE_REGISTER ||
        (options.match != IRIS_FEATURE_MATCH_GLOB && options.match != IRIS_FEATURE_MATCH_REGEX) ||
        (options.flags & ~(IRIS_FEATURE_LIST_AVAILABLE_ONLY | IRIS_FEATURE_LIST_INCLUDE_INTERNAL)))
        return invalid(operation);

    return api(operation, [&]() -> iris_status_t {
        std::regex pattern;
        if (options.name_pattern && options.match == IRIS_FEATURE_MATCH_REGEX) {
            try {
                pattern.assign(options.name_pattern);
            } catch (const std::regex_error &error) {
                return fail(IRIS_INVALID_ARGUMENT, operation, error.what());
            }
        }
        std::lock_guard lock(*map->mutex);
        GenApi::NodeList_t nodes;
        map->nodes._GetNodes(nodes);
        auto list = std::make_unique<iris_feature_list>();
        for (auto *node : nodes) {
            if (!(options.flags & IRIS_FEATURE_LIST_INCLUDE_INTERNAL) && !node->IsFeature()) continue;
            if (options.name_pattern) {
                const auto name = node->GetName();
                const bool matches = options.match == IRIS_FEATURE_MATCH_REGEX
                    ? std::regex_search(name.c_str(), pattern)
                    : fnmatch(options.name_pattern, name.c_str(), 0) == 0;
                if (!matches) continue;
            }
            auto entry = describe_entry(*node);
            if (options.type && entry.info.type != options.type) continue;
            if ((options.flags & IRIS_FEATURE_LIST_AVAILABLE_ONLY) &&
                (entry.info.access == IRIS_FEATURE_NOT_AVAILABLE || entry.info.access == IRIS_FEATURE_NOT_IMPLEMENTED))
                continue;
            list->entries.push_back(std::move(entry));
        }
        std::sort(list->entries.begin(), list->entries.end(), [](const auto &a, const auto &b) {
            return a.name < b.name;
        });
        *out = list.release();
        return IRIS_OK;
    });
}

size_t iris_feature_list_count(const iris_feature_list_t *list)
{
    return list ? list->entries.size() : 0;
}

iris_status_t iris_feature_list_get(const iris_feature_list_t *list, size_t index,
                                    iris_feature_entry_t *out)
{
    if (!list || !out || index >= list->entries.size()) return invalid("feature list get");
    const auto &entry = list->entries[index];
    *out = {entry.name.c_str(), entry.display_name.c_str(), entry.description.c_str(),
            entry.tooltip.c_str(), entry.info, entry.visibility};
    return IRIS_OK;
}

void iris_feature_list_close(iris_feature_list_t **list)
{
    if (list) {
        delete *list;
        *list = nullptr;
    }
}

iris_status_t iris_feature_info(iris_node_map_t *m, const char *n, iris_feature_info_t *out)
{
    if (!out) return invalid("feature info");
    return feature<GenApi::INode>(m, n, "feature info", [&](auto &v) {
        *out = describe_feature(v);
        return IRIS_OK;
    });
}

iris_status_t iris_feature_alias(iris_node_map_t *map, const char *name, char *out, size_t *size)
{
    constexpr auto operation = "feature alias";
    if (!size || (!out && *size)) return invalid(operation);
    return feature<GenApi::INode>(map, name, operation, [&](auto &node) {
        // GenApi 3.5.0 can dereference a missing pAlias in GetAlias().
        GenICam::gcstring value, attributes;
        require(node.GetProperty("pAlias", value, attributes) && !value.empty(),
                IRIS_NOT_FOUND, operation, "feature has no alias");
        auto *alias = node.GetAlias();
        require(alias, IRIS_NOT_FOUND, operation, "feature has no alias");
        return copy_text(alias->GetName().c_str(), out, size);
    });
}

iris_status_t iris_feature_event_id(iris_node_map_t *map, const char *name, char *out, size_t *size)
{
    constexpr auto operation = "feature event ID";
    if (!size || (!out && *size)) return invalid(operation);
    return feature<GenApi::INode>(map, name, operation, [&](auto &node) {
        const auto direct = node.GetEventID();
        if (!direct.empty()) return copy_text(direct.c_str(), out, size);

        const auto event = event_id_with_dependencies(node);
        require(!event.ambiguous, IRIS_AMBIGUOUS, operation, "feature depends on multiple event IDs");
        require(!event.id.empty(), IRIS_NOT_FOUND, operation, "feature has no event ID");
        return copy_text(event.id.c_str(), out, size);
    });
}

iris_status_t iris_feature_string_max_length(iris_node_map_t *map, const char *name, size_t *length)
{
    constexpr auto operation = "feature string maximum length";
    if (!length) return invalid(operation);
    return feature<GenApi::IString>(map, name, operation, [&](auto &node) {
        const auto maximum = node.GetMaxLength();
        require(maximum >= 0, IRIS_NOT_SUPPORTED, operation, "maximum length unavailable");
        *length = static_cast<size_t>(maximum);
        return IRIS_OK;
    });
}

iris_status_t iris_feature_invalidate(iris_node_map_t *map, const char *name)
{
    return feature<GenApi::INode>(map, name, "invalidate feature", [](auto &node) {
        node.InvalidateNode();
        return IRIS_OK;
    });
}
#define NUMERIC(name, iface, type) \
iris_status_t iris_feature_get_##name(iris_node_map_t *m, const char *n, type *out) \
{ \
    if (!out) return invalid("get " #name); \
    return feature<GenApi::iface>(m, n, "get " #name, [&](auto &v) { \
        *out = v.GetValue(false, true); return IRIS_OK; \
    }); \
} \
iris_status_t iris_feature_set_##name(iris_node_map_t *m, const char *n, type value) \
{ \
    return feature<GenApi::iface>(m, n, "set " #name, [&](auto &v) { \
        v.SetValue(value, true); return IRIS_OK; \
    }); \
}
NUMERIC(integer, IInteger, int64_t)
NUMERIC(float, IFloat, double)
#undef NUMERIC
#define RANGE(name, iface, type) \
iris_status_t iris_feature_##name##_range(iris_node_map_t *m, const char *n, iris_##name##_range_t *out) \
{ \
    if (!out) return invalid(#name " range"); \
    return feature<GenApi::iface>(m, n, #name " range", [&](auto &v) { \
        auto mode = increment_mode(v); \
        *out = {v.GetMin(), v.GetMax(), mode == IRIS_INCREMENT_FIXED ? v.GetInc() : type{}, mode}; \
        return IRIS_OK; \
    }); \
} \
iris_status_t iris_feature_##name##_values(iris_node_map_t *m, const char *n, type *out, size_t *count) \
{ \
    return feature<GenApi::iface>(m, n, #name " values", [&](auto &v) { \
        return feature_values(v, out, count, #name " values"); \
    }); \
}
RANGE(integer, IInteger, int64_t)
RANGE(float, IFloat, double)
#undef RANGE

iris_status_t iris_feature_get_boolean(iris_node_map_t *m, const char *n, uint32_t *out)
{
    if (!out) return invalid("get boolean");
    return feature<GenApi::IBoolean>(m, n, "get boolean", [&](auto &v) {
        *out = v.GetValue(false, true); return IRIS_OK;
    });
}
iris_status_t iris_feature_set_boolean(iris_node_map_t *m, const char *n, uint32_t value)
{
    if (value > 1) return invalid("set boolean");
    return feature<GenApi::IBoolean>(m, n, "set boolean", [&](auto &v) {
        v.SetValue(value != 0, true); return IRIS_OK;
    });
}
#define TEXT_GET(name, iface, expression) \
iris_status_t iris_feature_##name(iris_node_map_t *m, const char *n, char *out, size_t *size) \
{ \
    return feature<GenApi::iface>(m, n, #name, [&](auto &v) { \
        return copy_text((expression).c_str(), out, size); \
    }); \
}
#define TEXT_SET(name, iface, expression) \
iris_status_t iris_feature_##name(iris_node_map_t *m, const char *n, const char *value) \
{ \
    if (!value) return invalid(#name); \
    return feature<GenApi::iface>(m, n, #name, [&](auto &v) { expression; return IRIS_OK; }); \
}
TEXT_GET(to_string, IValue, v.ToString(false, true))
TEXT_GET(get_string, IString, v.GetValue(false, true))
TEXT_GET(get_enumeration, IEnumeration, v.GetCurrentEntry(false, true)->GetSymbolic())
TEXT_SET(from_string, IValue, v.FromString(value, true))
TEXT_SET(set_string, IString, v.SetValue(value, true))
TEXT_SET(set_enumeration, IEnumeration, v.FromString(value, true))
#undef TEXT_GET
#undef TEXT_SET

iris_status_t iris_feature_write_batch(iris_node_map_t *map, const iris_feature_write_t *writes,
                                       size_t count, size_t *applied)
{
    constexpr auto operation = "write feature batch";
    if (!applied) return invalid(operation);
    *applied = 0;
    if (!map || (!writes && count)) return invalid(operation);
    if (!count) return IRIS_OK;
    return api(operation, [&]() -> iris_status_t {
        std::lock_guard lock(*map->mutex);
        for (; *applied < count; ++*applied) {
            const auto &write = writes[*applied];
            if (!write.name || !*write.name || !write.value) return invalid(operation);
            auto &value = feature_value<GenApi::IValue>(*map, write.name, operation);
            require(value.GetNode()->GetPrincipalInterfaceType() != GenApi::intfICommand,
                    IRIS_WRONG_TYPE, operation, write.name);
            value.FromString(write.value, true);
        }
        return IRIS_OK;
    });
}

iris_status_t iris_feature_register_info(iris_node_map_t *map, const char *name,
                                         iris_register_info_t *info)
{
    constexpr auto operation = "register info";
    if (!info) return invalid(operation);
    return feature<GenApi::IRegister>(map, name, operation, [&](auto &reg) {
        const auto address = reg.GetAddress(true);
        const auto length = register_length(reg, operation);
        require(address >= 0 && length <= INT64_MAX - address,
                IRIS_OUT_OF_RANGE, operation, "invalid register extent");
        *info = {static_cast<uint64_t>(address), static_cast<size_t>(length)};
        return IRIS_OK;
    });
}

iris_status_t iris_feature_read_register(iris_node_map_t *map, const char *name, void *data, size_t *size)
{
    constexpr auto operation = "read register";
    if (!size || (!data && *size)) return invalid(operation);
    return feature<GenApi::IRegister>(map, name, operation, [&](auto &reg) {
        return read_register(reg, data, size, operation);
    });
}

iris_status_t iris_feature_write_register(iris_node_map_t *map, const char *name,
                                          const void *data, size_t size)
{
    constexpr auto operation = "write register";
    if (!data) return invalid(operation);
    return feature<GenApi::IRegister>(map, name, operation, [&](auto &reg) {
        const auto length = register_length(reg, operation);
        require(size == static_cast<size_t>(length), IRIS_OUT_OF_RANGE, operation, "register length mismatch");
        reg.Set(static_cast<const uint8_t *>(data), length, true);
        return IRIS_OK;
    });
}

iris_status_t iris_feature_unit(iris_node_map_t *m, const char *n, char *out, size_t *size)
{
    return feature<GenApi::INode>(m, n, "feature unit", [&](auto &v) {
        if (auto *i = dynamic_cast<GenApi::IInteger *>(&v)) return copy_text(i->GetUnit().c_str(), out, size);
        if (auto *f = dynamic_cast<GenApi::IFloat *>(&v)) return copy_text(f->GetUnit().c_str(), out, size);
        return fail(IRIS_WRONG_TYPE, "feature unit", "expected numeric feature");
    });
}
iris_status_t iris_feature_enumeration_count(iris_node_map_t *m, const char *n, size_t *out)
{
    if (!out) return invalid("enumeration count");
    return feature<GenApi::IEnumeration>(m, n, "enumeration count", [&](auto &v) {
        GenApi::NodeList_t entries;
        v.GetEntries(entries);
        *out = entries.size();
        return IRIS_OK;
    });
}
iris_status_t iris_feature_enumeration_entry(iris_node_map_t *m, const char *n, size_t index,
                                             char *out, size_t *size, iris_feature_access_t *access)
{
    if (!access) return invalid("enumeration entry");
    return feature<GenApi::IEnumeration>(m, n, "enumeration entry", [&](auto &v) {
        GenApi::NodeList_t entries;
        v.GetEntries(entries);
        if (index >= entries.size()) return invalid("enumeration entry");
        auto *entry = dynamic_cast<GenApi::IEnumEntry *>(entries[index]);
        if (!entry) return fail(IRIS_GENAPI_ERROR, "enumeration entry", "invalid entry");
        *access = feature_access(entries[index]->GetAccessMode());
        return copy_text(entry->GetSymbolic().c_str(), out, size);
    });
}
iris_status_t iris_feature_execute(iris_node_map_t *m, const char *n)
{
    return feature<GenApi::ICommand>(m, n, "execute command", [&](auto &v) {
        v.Execute(true); return IRIS_OK;
    });
}
iris_status_t iris_feature_command_done(iris_node_map_t *m, const char *n, uint32_t *out)
{
    if (!out) return invalid("command done");
    return feature<GenApi::ICommand>(m, n, "command done", [&](auto &v) {
        *out = v.IsDone(); return IRIS_OK;
    });
}
