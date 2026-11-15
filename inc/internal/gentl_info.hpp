#pragma once

#include "producer.hpp"
#include "error.hpp"

#include <cstring>
#include <type_traits>

namespace iris {

using namespace GenTL;

template<class F>
std::string text_query(Producer &p, const char *operation, F f, bool optional = false)
{
    size_t size = 0;
    auto rc = f(nullptr, &size);
    if (optional && rc != GC_ERR_SUCCESS && rc != GC_ERR_BUFFER_TOO_SMALL) return {};
    if (rc != GC_ERR_BUFFER_TOO_SMALL) p.check(rc, operation);
    if (!size && optional) return {};
    require(size && size <= 1024 * 1024, IRIS_PRODUCER_ERROR, operation, "invalid string size");
    std::string result(size, '\xff');
    rc = f(result.data(), &size);
    if (optional && rc != GC_ERR_SUCCESS) return {};
    p.check(rc, operation);
    require(size && size <= result.size(), IRIS_PRODUCER_ERROR, operation, "invalid returned string size");
    const auto end = result.find('\0');
    require(end < size, IRIS_PRODUCER_ERROR, operation, "unterminated string");
    result.resize(end);
    return result;
}

template<class T> constexpr INFO_DATATYPE info_type()
{
    if constexpr (std::is_same_v<T, bool8_t>) return INFO_DATATYPE_BOOL8;
    else if constexpr (std::is_same_v<T, void *>) return INFO_DATATYPE_PTR;
    else if constexpr (std::is_same_v<T, int32_t>) return INFO_DATATYPE_INT32;
    else return INFO_DATATYPE_UINT64; // size_t and uint64_t are the same on this ABI
}

template<class T, class F> bool get_info(T &out, F f)
{
    INFO_DATATYPE type = 0;
    size_t size = sizeof(T);
    T value{};
    if (f(&type, &value, &size) != GC_ERR_SUCCESS || size != sizeof(T)) return false;
    if (type != info_type<T>() && !(std::is_same_v<T, uint64_t> && type == INFO_DATATYPE_SIZET))
        return false;
    out = value;
    return true;
}

} // namespace iris
