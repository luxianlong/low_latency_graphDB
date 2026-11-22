#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <vector>

namespace low_latency_graphdb {

using NodeId = std::uint64_t;
using LocalNodeId = std::uint32_t;

struct alignas(64) GraphNodeSlab {
    NodeId node_id{0};
    std::uint32_t count{0};
    std::uint32_t flags{0};
    std::array<LocalNodeId, 11> inline_neighbors{};
    std::uint32_t overflow_index{0};

    [[nodiscard]] bool has_overflow() const noexcept {
        return count > inline_neighbors.size();
    }
};

static_assert(sizeof(GraphNodeSlab) == 64);
static_assert(alignof(GraphNodeSlab) == 64);

class FixedSlabPool {
public:
    explicit FixedSlabPool(std::size_t capacity)
        : slots_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("FixedSlabPool: capacity must be positive");
        }
        free_slots_.reserve(capacity);
        for (auto& slot : slots_) {
            free_slots_.push_back(&slot);
        }
    }

    FixedSlabPool(const FixedSlabPool&) = delete;
    FixedSlabPool& operator=(const FixedSlabPool&) = delete;

    template <typename... Args>
    GraphNodeSlab* allocate(Args&&... args) {
        std::lock_guard lock(mutex_);
        if (free_slots_.empty()) {
            return nullptr;
        }
        GraphNodeSlab* slot = free_slots_.back();
        free_slots_.pop_back();
        return ::new (slot) GraphNodeSlab{std::forward<Args>(args)...};
    }

    void release(GraphNodeSlab* node) noexcept {
        if (node == nullptr) {
            return;
        }
        std::lock_guard lock(mutex_);
        node->~GraphNodeSlab();
        free_slots_.push_back(node);
    }

    [[nodiscard]] std::size_t available() const noexcept {
        std::lock_guard lock(mutex_);
        return free_slots_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<GraphNodeSlab> slots_;
    std::vector<GraphNodeSlab*> free_slots_;
};

}  // namespace low_latency_graphdb
