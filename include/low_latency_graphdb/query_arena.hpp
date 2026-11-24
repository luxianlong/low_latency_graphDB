#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#define LLG_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define LLG_NOINLINE __attribute__((noinline))
#else
#define LLG_NOINLINE
#endif

namespace low_latency_graphdb {

// Query-scoped linear (bump) allocator, see design doc 01.
//
// One large 64-byte aligned chunk (default 16 MiB) is allocated and
// pre-faulted once, intended per worker thread. A query only bumps a cursor
// and finishes with an O(1) reset(); there is no per-object free. The chunk
// capacity is the hard per-query byte quota: an allocation that does not fit
// throws STATUS_QUERY_MEMORY_LIMIT_EXCEEDED instead of growing.
//
// Not thread-safe: use one arena per thread (see thread_arena()).
class QueryArena {
public:
    static constexpr std::size_t kDefaultCapacity = 16U * 1024U * 1024U;
    static constexpr std::size_t kAlignment = 64;  // cache line
    static constexpr std::size_t kPageSize = 4096;
    static constexpr std::size_t kDefaultAlignment = 8;

    explicit QueryArena(std::size_t capacity = kDefaultCapacity)
        : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("QueryArena: capacity must be positive");
        }
        void* ptr = nullptr;
        if (posix_memalign(&ptr, kAlignment, capacity_) != 0) {
            throw std::bad_alloc();
        }
        buffer_ = static_cast<char*>(ptr);
        pre_fault();
    }

    QueryArena(const QueryArena&) = delete;
    QueryArena& operator=(const QueryArena&) = delete;
    QueryArena(QueryArena&&) = delete;
    QueryArena& operator=(QueryArena&&) = delete;

    ~QueryArena() { std::free(buffer_); }

    [[nodiscard]] std::size_t used_bytes() const noexcept { return offset_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    // Query Begin: the arena must be empty (previous query reset it).
    void begin_query() const {
        if (offset_ != 0) {
            throw std::logic_error("QueryArena: begin_query on a non-reset arena");
        }
    }

    // Kept out-of-line so vector::push_back slow paths do not inline allocation
    // and exception-handling code into tight traversal loops.
    LLG_NOINLINE void* allocate(std::size_t bytes, std::size_t alignment = kDefaultAlignment) {
        if (bytes == 0) {
            return nullptr;
        }
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument("QueryArena: alignment must be a power of two");
        }
        const auto current = reinterpret_cast<std::uintptr_t>(buffer_) + offset_;
        const auto aligned =
            (current + alignment - 1) & ~(static_cast<std::uintptr_t>(alignment) - 1);
        const std::size_t padding = static_cast<std::size_t>(aligned - current);
        const std::size_t remaining = capacity_ - offset_;
        if (padding > remaining || bytes > remaining - padding) {
            throw std::runtime_error("STATUS_QUERY_MEMORY_LIMIT_EXCEEDED");
        }
        offset_ += padding + bytes;
        return reinterpret_cast<void*>(aligned);
    }

    template <typename T, typename... Args>
    T* construct(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "QueryArena only constructs trivially destructible objects");
        void* memory = allocate(sizeof(T), alignof(T));
        return ::new (memory) T(std::forward<Args>(args)...);
    }

    // O(1): cursor back to zero. Debug builds poison the whole chunk.
    void reset() noexcept {
#ifndef NDEBUG
        std::memset(buffer_, 0xCD, capacity_);
#endif
        offset_ = 0;
    }

private:
    // Touch one byte per 4KB page so physical frames are mapped up front and
    // the query hot path never takes a minor page fault.
    void pre_fault() noexcept {
        for (std::size_t i = 0; i < capacity_; i += kPageSize) {
            buffer_[i] = 0;
        }
        buffer_[capacity_ - 1] = 0;
    }

    char* buffer_{nullptr};
    std::size_t capacity_;
    std::size_t offset_{0};
};

// Per-thread arena, created (and pre-faulted) on first use in each thread.
inline QueryArena& thread_arena() {
    static thread_local QueryArena arena;
    return arena;
}

// Query lifecycle guard: begin_query() on entry, O(1) reset() on exit
// (including when the query throws).
class ArenaScope {
public:
    explicit ArenaScope(QueryArena& arena) : arena_(arena) { arena_.begin_query(); }
    ArenaScope(const ArenaScope&) = delete;
    ArenaScope& operator=(const ArenaScope&) = delete;
    ~ArenaScope() { arena_.reset(); }

private:
    QueryArena& arena_;
};

// STL adapter: lets std::vector and friends draw from a QueryArena.
// deallocate() is a no-op; memory is reclaimed by QueryArena::reset().
template <typename T>
class ArenaAllocator {
public:
    using value_type = T;

    explicit ArenaAllocator(QueryArena* arena) noexcept : arena_(arena) {}

    template <typename U>
    ArenaAllocator(const ArenaAllocator<U>& other) noexcept : arena_(other.arena()) {}

    T* allocate(std::size_t n) {
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(arena_->allocate(n * sizeof(T), alignof(T)));
    }

    void deallocate(T*, std::size_t) noexcept {}

    [[nodiscard]] QueryArena* arena() const noexcept { return arena_; }

    template <typename U>
    bool operator==(const ArenaAllocator<U>& other) const noexcept {
        return arena_ == other.arena();
    }
    template <typename U>
    bool operator!=(const ArenaAllocator<U>& other) const noexcept {
        return arena_ != other.arena();
    }

private:
    QueryArena* arena_{nullptr};
};

}  // namespace low_latency_graphdb

#undef LLG_NOINLINE
