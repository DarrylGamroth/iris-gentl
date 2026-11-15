#pragma once

#include "feature.hpp"

#include <memory>

struct iris_event_snapshot {
    struct Value {
        std::string text;
        std::string diagnostic;
        iris_status_t status = IRIS_OK;
        int32_t native_code = 0;
        uint32_t native_code_valid = 0;
    };

    iris_event_snapshot_info_t info{};
    std::vector<uint8_t> data;
    iris_feature_list features;
    std::vector<Value> values;
};

namespace iris {
std::unique_ptr<iris_event_snapshot> capture_event_snapshot(GenApi::INodeMap &map,
                                                            const iris_event_snapshot_info_t &event);
}
