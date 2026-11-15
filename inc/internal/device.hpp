#pragma once

#include "cache.hpp"
#include "iris.h"

#include <GenTL.h>

#include <array>
#include <memory>
#include <string>

namespace iris { struct Producer; }

struct alignas(iris::cache_line_size) iris_device {
    std::shared_ptr<iris::Producer> producer;
    iris_context *context = nullptr;
    GenTL::IF_HANDLE iface = nullptr;
    GenTL::DEV_HANDLE handle = nullptr;
    std::string interface;
    size_t streams = 0;
    iris_stream *active = nullptr;
    GenTL::DS_HANDLE failed_stream = nullptr;
    std::array<std::unique_ptr<iris_node_map>, 2> maps;
    bool writable = false;
    bool closing = false;
};

namespace iris { iris_status_t close_failed_stream(iris_device &device); }
