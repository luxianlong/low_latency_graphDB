#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>

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

    // Never copies the payload. Elements are read in place; when the storage
    // page is not naturally aligned, per-element memcpy loads (a single
    // unaligned load instruction) replace the typed span access.
    void reset(const char* data, std::size_t size) {
        data_ = nullptr;
        count_ = 0;
        aligned_ = false;

        if (data == nullptr || size < kHeaderSize) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated header");
        }

        const std::uint32_t count = read_u32(data);
        const std::size_t target_bytes = checked_multiply(count, sizeof(NodeId));
        const std::size_t meta_bytes = checked_multiply(count, sizeof(EdgeLightMeta));
        const std::size_t expected = checked_add(kHeaderSize, checked_add(target_bytes, meta_bytes));
        if (expected > size) {
            throw std::invalid_argument("ZeroCopyAdjView: truncated adjacency payload");
        }

        data_ = data;
        count_ = count;
        targets_ = data + kHeaderSize;
        metas_ = targets_ + target_bytes;
        aligned_ = is_aligned(targets_, alignof(NodeId)) &&
                   is_aligned(metas_, alignof(EdgeLightMeta));
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    // True when typed spans (targets()/edge_metas()) are available.
    [[nodiscard]] bool is_zero_copy() const noexcept {
        return aligned_;
    }

    [[nodiscard]] NodeId target(std::size_t index) const {
        check_index(index);
        NodeId value{};
        std::memcpy(&value, targets_ + index * sizeof(NodeId), sizeof(value));
        return value;
    }

    [[nodiscard]] EdgeLightMeta meta(std::size_t index) const {
        check_index(index);
        EdgeLightMeta value{};
        std::memcpy(&value, metas_ + index * sizeof(EdgeLightMeta), sizeof(value));
        return value;
    }

    // Typed spans; only valid for naturally aligned payloads (is_zero_copy()).
    [[nodiscard]] std::span<const NodeId> targets() const {
        require_aligned();
        return {reinterpret_cast<const NodeId*>(targets_), count_};
    }

    [[nodiscard]] std::span<const EdgeLightMeta> edge_metas() const {
        require_aligned();
        return {reinterpret_cast<const EdgeLightMeta*>(metas_), count_};
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

    void check_index(std::size_t index) const {
        if (index >= count_) {
            throw std::out_of_range("ZeroCopyAdjView: index out of bounds");
        }
    }

    void require_aligned() const {
        if (!aligned_) {
            throw std::logic_error("ZeroCopyAdjView: payload is unaligned; use target(i)/meta(i)");
        }
    }

    const char* data_{nullptr};
    const char* targets_{nullptr};
    const char* metas_{nullptr};
    std::size_t count_{0};
    bool aligned_{false};
};

}  // namespace low_latency_graphdb
