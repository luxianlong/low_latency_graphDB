#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

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
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count_;
    }

    // True when typed spans (targets()/edge_metas()) are available.
    [[nodiscard]] bool is_zero_copy() const noexcept {
        return data_ != nullptr && is_aligned(targets_ptr(), alignof(NodeId));
    }

    [[nodiscard]] NodeId target(std::size_t index) const {
        check_index(index);
        NodeId value{};
        std::memcpy(&value, targets_ptr() + index * sizeof(NodeId), sizeof(value));
        return value;
    }

    [[nodiscard]] EdgeLightMeta meta(std::size_t index) const {
        check_index(index);
        EdgeLightMeta value{};
        std::memcpy(&value, metas_ptr() + index * sizeof(EdgeLightMeta), sizeof(value));
        return value;
    }

    // Typed spans; only valid for naturally aligned payloads (is_zero_copy()).
    [[nodiscard]] std::span<const NodeId> targets() const {
        require_aligned();
        return {reinterpret_cast<const NodeId*>(targets_ptr()), count_};
    }

    [[nodiscard]] std::span<const EdgeLightMeta> edge_metas() const {
        require_aligned();
        return {reinterpret_cast<const EdgeLightMeta*>(metas_ptr()), count_};
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
        if (!is_zero_copy()) {
            throw std::logic_error("ZeroCopyAdjView: payload is unaligned; use target(i)/meta(i)");
        }
    }

    // targets/metas pointers and alignment are derived from data_ + count_, so
    // the view stays 16 bytes and trivially copyable (passed in two registers
    // under the System V AMD64 ABI). targets and metas share 8-byte alignment
    // because the target array is a multiple of 8 bytes long.
    [[nodiscard]] const char* targets_ptr() const noexcept { return data_ + kHeaderSize; }
    [[nodiscard]] const char* metas_ptr() const noexcept {
        return data_ + kHeaderSize + static_cast<std::size_t>(count_) * sizeof(NodeId);
    }

    const char* data_{nullptr};
    std::uint32_t count_{0};
    [[maybe_unused]] std::uint32_t reserved_{0};
};

static_assert(sizeof(ZeroCopyAdjView) == 16, "ZeroCopyAdjView must stay a 16-byte register-passed view");
static_assert(std::is_trivially_copyable_v<ZeroCopyAdjView>);

}  // namespace low_latency_graphdb
