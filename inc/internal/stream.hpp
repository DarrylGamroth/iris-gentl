#pragma once

#include "cache.hpp"
#include "iris.h"

#include <GenTL.h>

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>

namespace iris {

struct ChunkPart {
    const unsigned char *data = nullptr;
    size_t size = 0;
};

struct alignas(cache_line_size) Buffer {
    GenTL::BUFFER_HANDLE handle = nullptr;
    void *storage = nullptr; // Iris-owned fallback storage.
    void *caller_data = nullptr;
    void *user_data = nullptr;
    uint64_t token = 0;
    size_t capacity = 0;
    const unsigned char *payload_base = nullptr;
    size_t payload_bound = 0;
    size_t size_filled = 0;
    size_t data_size = 0;
    size_t part_count = 0;
    iris_payload_type_t payload_type = 0;
    ChunkPart chunk_part;
    bool has_size_filled = false;
    bool has_data_size = false;
    bool complete = false;
    bool contains_chunks = false;
    bool valid_parts = false;
    // Readers can retain these descriptors while the stream touches other slots.
    alignas(cache_line_size) std::array<iris_part_t, IRIS_MAX_FRAME_PARTS> parts;
};

static_assert(offsetof(Buffer, parts) % cache_line_size == 0);

void inspect_parts(iris_stream &stream, Buffer &buffer, iris_frame_t &frame,
                   const unsigned char *base, size_t capacity);

} // namespace iris

struct alignas(iris::cache_line_size) iris_stream {
    std::mutex mutex;
    size_t requeues = 0;
    bool reader = false;
    bool running = false;
    bool stopping = false;
    bool remote = false;
    bool host = false;
    bool locked = false;
    bool flush = false;
    bool event_registered = false;
    std::condition_variable idle;
    iris_stream_stats_t stats{};
    iris_device *device = nullptr;
    GenTL::DS_HANDLE handle = nullptr;
    GenTL::EVENT_HANDLE event = nullptr;
    std::unique_ptr<iris_node_map> map;
    iris_node_map *remote_map = nullptr;
    std::vector<iris::Buffer> buffers;
    struct {
        iris::Buffer *buffer = nullptr;
        iris_row_target_t target{};
        const void *base = nullptr;
        uint32_t rows = 0;
    } partial;
};
