#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace low_latency_graphdb {

class QueryArena {
public:
    static constexpr std::size_t kDefaultPrimaryCapacity = 4U * 1024U * 1024U;
    static constexpr std::size_t kDefaultQuota = 16U * 1024U * 1024U;
    static constexpr std::size_t kAlignment = 64;

    explicit QueryArena(
        std::size_t primary_capacity = kDefaultPrimaryCapacity,
        std::size_t quota = kDefaultQuota)
        : primary_capacity_(primary_capacity),
          quota_(quota) {
        if (primary_capacity_ == 0 || quota_ < primary_capacity_) {
            throw std::invalid_argument("QueryArena: invalid capacity or quota");
        }
        primary_ = allocate_chunk(primary_capacity_);
    }

    QueryArena(const QueryArena&) = delete;
    QueryArena& operator=(const QueryArena&) = delete;
    QueryArena(QueryArena&&) = delete;
    QueryArena& operator=(QueryArena&&) = delete;

    ~QueryArena() = default;

    [[nodiscard]] std::size_t used_bytes() const noexcept {
        return used_bytes_;
    }

    [[nodiscard]] std::size_t quota() const noexcept {
        return quota_;
    }

    void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
        if (bytes == 0) {
            return nullptr;
        }
        validate_alignment(alignment);

        const std::size_t primary_result = try_allocate(primary_.get(), bytes, alignment);
        if (primary_result != kNoAllocation) {
            used_bytes_ = checked_add(used_bytes_, primary_result);
            return primary_->data.get() + primary_->offset - bytes;
        }

        const std::size_t chunk_capacity = std::max(bytes, primary_capacity_);
        const std::size_t chunk_cost = checked_add(chunk_capacity, alignment - 1);
        ensure_quota(chunk_cost);
        auto chunk = allocate_chunk(chunk_capacity);
        const std::size_t allocated = try_allocate(chunk.get(), bytes, alignment);
        if (allocated == kNoAllocation) {
            throw std::bad_alloc();
        }
        void* result = chunk->data.get() + chunk->offset - bytes;
        used_bytes_ = checked_add(used_bytes_, allocated);
        overflow_.push_back(std::move(chunk));
        return result;
    }

    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "QueryArena only constructs trivially destructible objects");
        void* memory = allocate(sizeof(T), alignof(T));
        return ::new (memory) T(std::forward<Args>(args)...);
    }

    void reset() noexcept {
#ifndef NDEBUG
        poison(*primary_);
        for (const auto& chunk : overflow_) {
            poison(*chunk);
        }
#endif
        primary_->offset = 0;
        overflow_.clear();
        used_bytes_ = 0;
    }

private:
    struct Chunk {
        std::unique_ptr<std::byte[]> data;
        std::size_t capacity{0};
        std::size_t offset{0};
    };

    static constexpr std::size_t kNoAllocation = std::numeric_limits<std::size_t>::max();

    static void validate_alignment(std::size_t alignment) {
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument("QueryArena: alignment must be a power of two");
        }
    }

    static std::size_t checked_add(std::size_t left, std::size_t right) {
        if (right > std::numeric_limits<std::size_t>::max() - left) {
            throw std::bad_alloc();
        }
        return left + right;
    }

    void ensure_quota(std::size_t additional) const {
        if (additional > quota_ - used_bytes_) {
            throw std::runtime_error("STATUS_QUERY_MEMORY_LIMIT_EXCEEDED");
        }
    }

    static std::unique_ptr<Chunk> allocate_chunk(std::size_t capacity) {
        auto chunk = std::make_unique<Chunk>();
        chunk->data = std::make_unique<std::byte[]>(capacity + kAlignment - 1);
        chunk->capacity = capacity + kAlignment - 1;
        return chunk;
    }

    static std::size_t try_allocate(
        Chunk* chunk,
        std::size_t bytes,
        std::size_t alignment) noexcept {
        const auto base = reinterpret_cast<std::uintptr_t>(chunk->data.get());
        const auto current = base + chunk->offset;
        const auto aligned = (current + alignment - 1) & ~(alignment - 1);
        const std::size_t padding = static_cast<std::size_t>(aligned - current);
        if (padding > chunk->capacity - chunk->offset ||
            bytes > chunk->capacity - chunk->offset - padding) {
            return kNoAllocation;
        }
        chunk->offset += padding + bytes;
        return padding + bytes;
    }

#ifndef NDEBUG
    static void poison(const Chunk& chunk) noexcept {
        std::fill_n(chunk.data.get(), chunk.capacity, std::byte{0xCD});
    }
#else
    static void poison(const Chunk&) noexcept {}
#endif

    std::size_t primary_capacity_;
    std::size_t quota_;
    std::size_t used_bytes_{0};
    std::unique_ptr<Chunk> primary_;
    std::vector<std::unique_ptr<Chunk>> overflow_;
};

}  // namespace low_latency_graphdb
