#pragma once

#include "iris.h"

#include <GenApi/GenApi.h>

#include <string>
#include <vector>

namespace iris {

struct FeatureEntry {
    std::string name, display_name, description, tooltip;
    iris_feature_info_t info;
    iris_feature_visibility_t visibility;
};

struct FeatureEventID {
    std::string id;
    bool ambiguous = false;
};

FeatureEntry describe_entry(GenApi::INode &node);
FeatureEventID event_id_with_dependencies(GenApi::INode &node);
iris_status_t copy_text(const char *text, char *out, size_t *size);
iris_status_t read_register(GenApi::IRegister &reg, void *data, size_t *size, const char *operation);

} // namespace iris

struct iris_feature_list {
    std::vector<iris::FeatureEntry> entries;
};
