#include "event_snapshot.hpp"

#include "error.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <string_view>
#include <strings.h>

using namespace iris;

namespace {

bool matches(const FeatureEventID &feature, const iris_node_map_event_t &event)
{
    if (feature.ambiguous || feature.id.empty()) return false;
    if (!event.has_numeric_id) return strcasecmp(feature.id.c_str(), event.id) == 0;
    uint64_t value = 0;
    const auto end = feature.id.data() + feature.id.size();
    const auto parsed = std::from_chars(feature.id.data(), end, value, 16);
    return parsed.ec == std::errc{} && parsed.ptr == end && value == event.numeric_id;
}

bool timestamp_name(std::string_view name)
{
    constexpr std::string_view prefix = "Event", suffix = "Timestamp";
    return name.size() > prefix.size() + suffix.size() &&
        name.substr(0, prefix.size()) == prefix && name.substr(name.size() - suffix.size()) == suffix;
}

struct EventFeature {
    FeatureEntry entry;
    GenApi::IValue *value;
};

} // namespace

std::unique_ptr<iris_event_snapshot> iris::capture_event_snapshot(GenApi::INodeMap &map,
                                                                 const iris_event_snapshot_info_t &info)
{
    auto snapshot = std::make_unique<iris_event_snapshot>();
    snapshot->info = info;
    snapshot->info.data = nullptr;
    if (info.event.data_size) {
        const auto *data = static_cast<const uint8_t *>(info.data);
        snapshot->data.assign(data, data + info.event.data_size);
    }

    GenApi::NodeList_t nodes;
    map.GetNodes(nodes);
    std::vector<EventFeature> matches_for_event;
    GenApi::IInteger *timestamp = nullptr;
    bool ambiguous_timestamp = false;
    for (auto *node : nodes) {
        auto *value = dynamic_cast<GenApi::IValue *>(node);
        const auto type = node->GetPrincipalInterfaceType();
        if (!value || type == GenApi::intfICategory || type == GenApi::intfICommand ||
            !matches(event_id_with_dependencies(*node), info.event))
            continue;

        auto entry = describe_entry(*node);
        const bool readable = entry.info.access == IRIS_FEATURE_READ_ONLY ||
                              entry.info.access == IRIS_FEATURE_READ_WRITE;
        if (!readable) continue;
        if (timestamp_name(entry.name)) {
            if (auto *integer = dynamic_cast<GenApi::IInteger *>(node)) {
                ambiguous_timestamp |= timestamp != nullptr;
                timestamp = integer;
            }
        }
        matches_for_event.push_back({std::move(entry), value});
    }
    std::sort(matches_for_event.begin(), matches_for_event.end(), [](const auto &a, const auto &b) {
        return a.entry.name < b.entry.name;
    });
    snapshot->features.entries.reserve(matches_for_event.size());
    snapshot->values.reserve(matches_for_event.size());
    for (auto &feature : matches_for_event) {
        iris_event_snapshot::Value value;
        value.status = api("event feature", [&] {
            value.text = feature.value->ToString(true, true).c_str();
            return IRIS_OK;
        });
        if (value.status == IRIS_NO_MEMORY) throw Failure{value.status};
        if (value.status < 0) {
            value.diagnostic = last.message;
            value.native_code = last.native;
            value.native_code_valid = last.valid;
        }
        snapshot->features.entries.push_back(std::move(feature.entry));
        snapshot->values.push_back(std::move(value));
    }
    if (timestamp && !ambiguous_timestamp) {
        const auto status = api("event timestamp", [&] {
            snapshot->info.device_timestamp = static_cast<uint64_t>(timestamp->GetValue(true, true));
            return IRIS_OK;
        });
        if (status == IRIS_NO_MEMORY) throw Failure{status};
        if (status == IRIS_OK) snapshot->info.valid |= IRIS_EVENT_HAS_DEVICE_TIMESTAMP;
    }
    return snapshot;
}

iris_status_t iris_event_snapshot_info(const iris_event_snapshot_t *snapshot, iris_event_snapshot_info_t *info)
{
    if (!snapshot || !info) return invalid("event snapshot info");
    *info = snapshot->info;
    if (!snapshot->data.empty()) info->data = snapshot->data.data();
    return IRIS_OK;
}

const iris_feature_list_t *iris_event_snapshot_features(const iris_event_snapshot_t *snapshot)
{
    return snapshot ? &snapshot->features : nullptr;
}

iris_status_t iris_event_snapshot_feature_to_string(const iris_event_snapshot_t *snapshot, const char *name,
                                                    char *text, size_t *size)
{
    constexpr auto operation = "event feature";
    if (!snapshot || !name || !*name || !size || (!text && *size)) return invalid(operation);
    const auto &entries = snapshot->features.entries;
    const auto entry = std::lower_bound(entries.begin(), entries.end(), name,
                                       [](const auto &feature, const char *target) {
        return feature.name.compare(target) < 0;
    });
    if (entry == entries.end() || entry->name != name) return fail(IRIS_NOT_FOUND, operation, name);
    const auto &value = snapshot->values[entry - entries.begin()];
    if (value.status < 0) {
        fail(value.status, operation, value.diagnostic.c_str());
        last.native = value.native_code;
        last.valid = value.native_code_valid;
        return value.status;
    }
    return copy_text(value.text.c_str(), text, size);
}

void iris_event_snapshot_close(iris_event_snapshot_t **snapshot)
{
    if (snapshot) {
        delete *snapshot;
        *snapshot = nullptr;
    }
}
