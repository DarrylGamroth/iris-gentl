#include "error.hpp"

#include <cstdio>

namespace iris {

thread_local Error last;

iris_status_t fail(iris_status_t status, const char *operation, const char *message)
{
    last = {};
    last.operation = operation;
    std::snprintf(last.message, sizeof(last.message), "%s", message);
    return status;
}

void require(bool condition, iris_status_t status, const char *operation, const char *message)
{
    if (!condition) throw Failure{fail(status, operation, message)};
}

iris_status_t invalid(const char *operation)
{
    return fail(IRIS_INVALID_ARGUMENT, operation, "invalid argument");
}

} // namespace iris

using iris::last;

uint32_t iris_abi_major(void) { return IRIS_ABI_MAJOR; }
void iris_last_error(iris_error_t *out)
{
    if (out) *out = {last.operation, last.message, last.native, last.valid};
}
