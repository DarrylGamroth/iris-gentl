#pragma once

#include "iris.h"

#include <Base/GCException.h>
#include <exception>
#include <new>

namespace iris {

struct Error {
    const char *operation = "";
    char message[1024]{};
    int32_t native = 0;
    uint32_t valid = 0;
};

extern thread_local Error last;
iris_status_t fail(iris_status_t status, const char *operation, const char *message);
iris_status_t invalid(const char *operation);
void require(bool condition, iris_status_t status, const char *operation, const char *message);

struct Failure { iris_status_t status; };

template<class F>
iris_status_t api(const char *operation, F f) noexcept
{
    try { return f(); }
    catch (const Failure &e) { return e.status; }
    catch (const GenICam::AccessException &e) { return fail(IRIS_ACCESS_DENIED, operation, e.GetDescription()); }
    catch (const GenICam::OutOfRangeException &e) { return fail(IRIS_OUT_OF_RANGE, operation, e.GetDescription()); }
    catch (const GenICam::GenericException &e) { return fail(IRIS_GENAPI_ERROR, operation, e.GetDescription()); }
    catch (const std::bad_alloc &) { return fail(IRIS_NO_MEMORY, operation, "allocation failed"); }
    catch (const std::exception &e) { return fail(IRIS_IO_ERROR, operation, e.what()); }
    catch (...) { return fail(IRIS_IO_ERROR, operation, "unexpected exception"); }
}

} // namespace iris
