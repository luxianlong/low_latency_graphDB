#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace low_latency_graphdb {

struct EdgeLightMeta {
    std::uint16_t edge_type{0};
    std::uint16_t weight{0};
    std::uint32_t updated_timestamp{0};
};

static_assert(sizeof(EdgeLightMeta) == 8);

class ZeroCopyAdjView {
public:
    using NodeId = std::uint64_t;
    static constexpr std::size_t kHeaderSize = sizeof(std::uint32_t);

    ZeroCopyAdjView() = default;

    ZeroCopyAdjView(const char* data, std::size_t size) {
        reset(data, size);
    }

    void reset(const char* data, std::size_t size) {
        data_ = data;
        size_ = size;
        target_ids_ = {};
        edge_metas_ = {};
        fallback_targets_.clear();
        fallback_metas_.clear();
        is_zero_copy_ = false;

        if (data_ == nullptr || size_ < kHeaderSize) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated header");
        }

        const std::uint32_t count = read_u32(data_);
        const std::size_t target_bytes = checked_multiply(count, sizeof(NodeId));
        const std::size_t meta_bytes = checked_multiply(count, sizeof(EdgeLightMeta));
        const std::size_t expected = checked_add(kHeaderSize, checked_add(target_bytes, meta_bytes));
        if (expected > size_) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated adjacency payload");
        }

        const char* targets = data_ + kHeaderSize;
        const char* metas = targets + target_bytes;
        if (is_aligned(targets, alignof(NodeId)) &&
            is_aligned(metas, alignof(EdgeLightMeta))) {
            target_ids_ = std::span<const NodeId>(
                reinterpret_cast<const NodeId*>(targets), count);
            edge_metas_ = std::span<const EdgeLightMeta>(
                reinterpret_cast<const EdgeLightMeta*>(metas), count);
            is_zero_copy_ = true;
            return;
        }

        fallback_targets_.resize(count);
        fallback_metas_.resize(count);
        std::memcpy(fallback_targets_.data(), targets, target_bytes);
        std::memcpy(fallback_metas_.data(), metas, meta_bytes);
        target_ids_ = fallback_targets_;
        edge_metas_ = fallback_metas_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return target_ids_.size();
    }

    [[nodiscard]] bool is_zero_copy() const noexcept {
        return is_zero_copy_;
    }

    [[nodiscard]] NodeId target(std::size_t index) const {
        if (index >= target_ids_.size()) {
            throw std::out_of_range("ZeroCopyAdjView: target index out of bounds");
        }
        if (is_zero_copy_) {
            return target_ids_[index];
        }
        NodeId value{};
        const char* source = data_ + kHeaderSize + index * sizeof(NodeId);
        std::memcpy(&value, source, sizeof(value));
        return value;
    }

    [[nodiscard]] std::span<const NodeId> targets() const noexcept {
        return target_ids_;
    }

    [[nodiscard]] std::span<const EdgeLightMeta> edge_metas() const noexcept {
        return edge_metas_;
    }

private:
    static bool is_aligned(const char* address, std::size_t alignment) noexcept {
        return reinterpret_cast<std::uintptr_t>(address) % alignment == 0;
    }

    static std::uint32_t read_u32(const char* data) noexcept {
        std::uint32_t value{};
        std::memcpy(&value, data, sizeof(value));
        return value;
    }

    static std::size_t checked_add(std::size_t left, std::size_t right) {
        if (right > std::numeric_limits<std::size_t>::max() - left) {
            throw std::invalid_argument("ZeroCopyAdjView: size overflow");
        }
        return left + right;
    }

    static std::size_t checked_multiply(std::size_t left, std::size_t right) {
        if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
            throw std::invalid_argument("ZeroCopyAdjView: size overflow");
        }
        return left * right;
    }

    const char* data_{nullptr};
    std::size_t size_{0};
    std::span<const NodeId> target_ids_{};
    std::span<const EdgeLightMeta> edge_metas_{};
    std::vector<NodeId> fallback_targets_;
    std::vector<EdgeLightMeta> fallback_metas_;
    bool is_zero_copy_{false};
};

}  // namespace low_latency_graphdb
