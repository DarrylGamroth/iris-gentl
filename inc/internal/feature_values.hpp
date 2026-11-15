#pragma once

#include "error.hpp"

#include <GenApi/Types.h>

namespace iris {

template<class Node, class T>
iris_status_t feature_values(Node &node, T *out, size_t *count, const char *operation)
{
    if (!count || (!out && *count)) return invalid(operation);
    if (node.GetIncMode() != GenApi::listIncrement)
        return fail(IRIS_NOT_SUPPORTED, operation, "feature has no value list");
    const auto values = node.GetListOfValidValues();
    const size_t capacity = *count;
    *count = values.size();
    if (!out) return IRIS_OK;
    if (capacity < *count) return fail(IRIS_BUFFER_TOO_SMALL, operation, "output array too small");
    for (size_t i = 0; i < *count; ++i) out[i] = values[i];
    return IRIS_OK;
}

} // namespace iris
