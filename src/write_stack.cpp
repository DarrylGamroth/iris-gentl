#include "node_map.hpp"

#include "error.hpp"
#include "producer.hpp"

#include <limits>

namespace iris {

void Port::Write(GenApi::PORT_REGISTER_STACK_ENTRY *entries, size_t count)
{
    constexpr auto operation = "GCWritePortStacked";
    if (!count) return;

    std::vector<GenTL::PORT_REGISTER_STACK_ENTRY> writes;
    if (stacked_written) writes.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &entry = entries[i];
        constexpr auto max_address = static_cast<uint64_t>(INT64_MAX);
        require(entry.Address <= max_address && entry.Size <= max_address - entry.Address,
                IRIS_OUT_OF_RANGE, operation, "invalid register extent");
        if (stacked_written) writes.push_back({entry.Address, entry.pBuffer, entry.Size});
        else Write(entry.pBuffer, static_cast<int64_t>(entry.Address), static_cast<int64_t>(entry.Size));
    }
    if (!stacked_written) return;

    size_t completed = count;
    const auto status = producer.GCWritePortStacked(handle, writes.data(), &completed);
    // An unsupported-operation stub can leave the input count untouched.
    if (completed == count && (status == GenTL::GC_ERR_NOT_IMPLEMENTED || status == GenTL::GC_ERR_NOT_AVAILABLE))
        completed = 0;
    require(completed <= count && completed <= std::numeric_limits<size_t>::max() - *stacked_written,
            IRIS_PRODUCER_ERROR, operation, "invalid stacked register count");
    *stacked_written += completed;
    producer.check(status, operation);
    require(completed == count, IRIS_PRODUCER_ERROR, operation, "short stacked register write");
}

} // namespace iris

using namespace iris;

iris_status_t iris_node_map_start_stacking(iris_node_map_t *map)
{
    constexpr auto operation = "start stacking";
    if (!map) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        require(!map->write_stack, IRIS_BUSY, operation, "node map already has an active stack");
        require(map->port.producer.GCWritePortStacked, IRIS_NOT_SUPPORTED, operation,
                "producer does not export GCWritePortStacked");
        require(GenApi::IsWritable(map->port.GetAccessMode()), IRIS_ACCESS_DENIED, operation,
                "node map port is not writable");
        map->write_stack.reset(map->nodes._NewNodeWriteConcatenator());
        require(static_cast<bool>(map->write_stack), IRIS_GENAPI_ERROR, operation,
                "GenApi did not create a write stack");
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_stack_add(iris_node_map_t *map, const char *name, const char *value)
{
    constexpr auto operation = "add stacked write";
    if (!map || !name || !*name || !value) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        require(static_cast<bool>(map->write_stack), IRIS_INVALID_STATE, operation, "no active stack");
        auto *node = map->nodes._GetNode(name);
        require(node, IRIS_NOT_FOUND, operation, name);
        require(dynamic_cast<GenApi::IValue *>(node) &&
                node->GetPrincipalInterfaceType() != GenApi::intfICommand,
                IRIS_WRONG_TYPE, operation, name);
        map->write_stack->Add(name, value);
        return IRIS_OK;
    });
}

iris_status_t iris_node_map_write_stack(iris_node_map_t *map, size_t *registers_written)
{
    constexpr auto operation = "write stack";
    if (!registers_written) return invalid(operation);
    *registers_written = 0;
    if (!map) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        require(static_cast<bool>(map->write_stack), IRIS_INVALID_STATE, operation, "no active stack");
        auto stack = std::move(map->write_stack);
        map->port.stacked_written = registers_written;
        const auto status = api(operation, [&]() -> iris_status_t {
            GenICam::gcstring_vector errors;
            if (!map->nodes._ConcatenatedWrite(stack.get(), false, &errors))
                return fail(IRIS_GENAPI_ERROR, operation, errors.empty() ? "GenApi stack failed" : errors[0].c_str());
            return IRIS_OK;
        });
        map->port.stacked_written = nullptr;
        const auto diagnostic = last;
        const auto invalidated = api("invalidate node map", [&] {
            map->nodes._InvalidateNodes();
            return IRIS_OK;
        });
        if (status < 0) {
            last = diagnostic;
            return status;
        }
        return invalidated;
    });
}

iris_status_t iris_node_map_cancel_stack(iris_node_map_t *map)
{
    constexpr auto operation = "cancel stack";
    if (!map) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        map->write_stack.reset();
        return IRIS_OK;
    });
}
