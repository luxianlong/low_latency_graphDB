#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace low_latency_graphdb {

using NodeId = std::uint64_t;
using LocalNodeId = std::uint32_t;

struct TraversalResult {
    std::vector<NodeId> visited;
};

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
static_assert(std::is_trivially_destructible_v<GraphNodeSlab>);

// Intrusive free-list slot (design doc 02): while free, the first 8 bytes store
// `next_free`; while allocated, the same 64 bytes hold a live `GraphNodeSlab`.
union alignas(64) SlabSlot {
    SlabSlot* next_free;
    GraphNodeSlab node;

    SlabSlot() noexcept : next_free(nullptr) {}
    ~SlabSlot() noexcept {}
};

static_assert(sizeof(SlabSlot) == 64);
static_assert(alignof(SlabSlot) == 64);

// Fixed-capacity 64-byte aligned slab pool with intrusive free-list (0-byte
// per-slot metadata) and 3-bucket EBR deferred reclamation (design doc 02).
class FixedSlabPool {
public:
    static constexpr std::size_t kAlignment = 64;
    static constexpr std::size_t kPageSize = 4096;
    static constexpr std::size_t kEpochBuckets = 3;

    explicit FixedSlabPool(std::size_t capacity)
        : capacity_(capacity), available_(capacity) {
        if (capacity_ == 0 ||
            capacity_ > std::numeric_limits<std::size_t>::max() / sizeof(SlabSlot)) {
            throw std::invalid_argument("FixedSlabPool: invalid capacity");
        }
        const std::size_t total_bytes = capacity_ * sizeof(SlabSlot);
        void* raw = nullptr;
        if (posix_memalign(&raw, kAlignment, total_bytes) != 0 || raw == nullptr) {
            throw std::bad_alloc();
        }
        pre_fault(static_cast<char*>(raw), total_bytes);

        slots_ = static_cast<SlabSlot*>(raw);
        for (std::size_t i = 0; i + 1 < capacity_; ++i) {
            slots_[i].next_free = &slots_[i + 1];
        }
        slots_[capacity_ - 1].next_free = nullptr;
        free_head_ = &slots_[0];
    }

    FixedSlabPool(const FixedSlabPool&) = delete;
    FixedSlabPool& operator=(const FixedSlabPool&) = delete;
    FixedSlabPool(FixedSlabPool&&) = delete;
    FixedSlabPool& operator=(FixedSlabPool&&) = delete;

    ~FixedSlabPool() {
        std::free(slots_);
    }

    template <typename... Args>
    GraphNodeSlab* allocate(Args&&... args) {
        std::lock_guard lock(mutex_);
        if (free_head_ == nullptr) {
            return nullptr;
        }
        SlabSlot* slot = free_head_;
        free_head_ = slot->next_free;
        --available_;
        return ::new (static_cast<void*>(&slot->node)) GraphNodeSlab{std::forward<Args>(args)...};
    }

    // Immediate recycle (caller guarantees no concurrent reader holds `node`).
    void release(GraphNodeSlab* node) noexcept {
        if (node == nullptr) {
            return;
        }
        std::lock_guard lock(mutex_);
        recycle_locked(reinterpret_cast<SlabSlot*>(node));
    }

    // EBR deferred recycle: parks `node` in the bucket for `current_epoch`
    // without reusing it until `reclaim_epoch(safe_epoch)` runs.
    void retire_slot(GraphNodeSlab* node, std::uint64_t current_epoch) {
        if (node == nullptr) {
            return;
        }
        std::lock_guard lock(mutex_);
        auto* slot = reinterpret_cast<SlabSlot*>(node);
        retire_buckets_[current_epoch % kEpochBuckets].push_back(slot);
        ++retired_;
    }

    // Reclaims every slot retired in `safe_epoch`, invoking `on_reclaim(node)`
    // before destructing, poisoning in Debug builds, and relinking into free-list.
    template <typename OnReclaim>
    std::size_t reclaim_epoch(std::uint64_t safe_epoch, OnReclaim&& on_reclaim) noexcept {
        std::lock_guard lock(mutex_);
        auto& bucket = retire_buckets_[safe_epoch % kEpochBuckets];
        const std::size_t count = bucket.size();
        for (SlabSlot* slot : bucket) {
            on_reclaim(slot->node);
            recycle_locked(slot);
        }
        bucket.clear();
        retired_ -= count;
        return count;
    }

    std::size_t reclaim_epoch(std::uint64_t safe_epoch) noexcept {
        return reclaim_epoch(safe_epoch, [](const GraphNodeSlab&) noexcept {});
    }

    [[nodiscard]] bool owns(const GraphNodeSlab* node) const noexcept {
        if (node == nullptr) {
            return false;
        }
        const auto addr = reinterpret_cast<std::uintptr_t>(node);
        const auto base = reinterpret_cast<std::uintptr_t>(slots_);
        const std::size_t total_bytes = capacity_ * sizeof(SlabSlot);
        return addr >= base && addr - base < total_bytes &&
               (addr - base) % sizeof(SlabSlot) == 0;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    [[nodiscard]] std::size_t available() const noexcept {
        std::lock_guard lock(mutex_);
        return available_;
    }

    [[nodiscard]] std::size_t retired_count() const noexcept {
        std::lock_guard lock(mutex_);
        return retired_;
    }

private:
    static void pre_fault(char* buffer, std::size_t bytes) noexcept {
        for (std::size_t i = 0; i < bytes; i += kPageSize) {
            buffer[i] = 0;
        }
        buffer[bytes - 1] = 0;
    }

    void recycle_locked(SlabSlot* slot) noexcept {
        slot->node.~GraphNodeSlab();
#ifndef NDEBUG
        std::memset(static_cast<void*>(slot), 0xCD, sizeof(SlabSlot));
#endif
        slot->next_free = free_head_;
        free_head_ = slot;
        ++available_;
    }

    mutable std::mutex mutex_;
    SlabSlot* slots_{nullptr};
    SlabSlot* free_head_{nullptr};
    std::size_t capacity_{0};
    std::size_t available_{0};
    std::size_t retired_{0};
    std::array<std::vector<SlabSlot*>, kEpochBuckets> retire_buckets_{};
};

// Lightweight 3-epoch EBR coordinator for `FixedSlabPool`.
// Readers publish their observed epoch into cache-line-isolated slots with no
// locks; writers call `retire()` after unlinking a slab and `try_advance()` to
// advance the global epoch and reclaim retired slots once all active readers
// have moved past the grace period.
class EpochTracker : public std::enable_shared_from_this<EpochTracker> {
public:
    static constexpr std::size_t kMaxReaders = 128;
    static constexpr std::uint64_t kInactive = std::numeric_limits<std::uint64_t>::max();

    class ReaderHandle {
    public:
        ReaderHandle() = default;
        ReaderHandle(const ReaderHandle&) = delete;
        ReaderHandle& operator=(const ReaderHandle&) = delete;
        ReaderHandle(ReaderHandle&& other) noexcept
            : owner_(std::move(other.owner_)),
              tracker_(other.tracker_),
              index_(other.index_) {
            other.tracker_ = nullptr;
            other.index_ = kMaxReaders;
        }
        ReaderHandle& operator=(ReaderHandle&& other) noexcept {
            if (this != &other) {
                release();
                owner_ = std::move(other.owner_);
                tracker_ = other.tracker_;
                index_ = other.index_;
                other.tracker_ = nullptr;
                other.index_ = kMaxReaders;
            }
            return *this;
        }
        ~ReaderHandle() { release(); }

        [[nodiscard]] bool valid() const noexcept { return tracker_ != nullptr; }
        [[nodiscard]] const EpochTracker* tracker() const noexcept { return tracker_; }

    private:
        friend class EpochTracker;
        ReaderHandle(std::shared_ptr<EpochTracker> owner, EpochTracker* tracker,
                     std::size_t index) noexcept
            : owner_(std::move(owner)), tracker_(tracker), index_(index) {}

        void release() noexcept {
            if (tracker_ != nullptr) {
                tracker_->unregister_reader(index_);
                tracker_ = nullptr;
                owner_.reset();
                index_ = kMaxReaders;
            }
        }

        std::shared_ptr<EpochTracker> owner_;
        EpochTracker* tracker_{nullptr};
        std::size_t index_{kMaxReaders};
    };

    class ReadGuard {
    public:
        ReadGuard(EpochTracker& tracker, const ReaderHandle& handle) noexcept
            : tracker_(tracker), index_(handle.index_) {
            tracker_.enter_read(index_);
        }
        ReadGuard(const ReadGuard&) = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
        ~ReadGuard() { tracker_.exit_read(index_); }

    private:
        EpochTracker& tracker_;
        std::size_t index_;
    };

    ReaderHandle register_reader() {
        std::shared_ptr<EpochTracker> owner;
        try {
            owner = shared_from_this();
        } catch (const std::bad_weak_ptr&) {
        }
        for (std::size_t i = 0; i < kMaxReaders; ++i) {
            bool expected = false;
            if (readers_[i].in_use.compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                return ReaderHandle(std::move(owner), this, i);
            }
        }
        throw std::runtime_error("EpochTracker: max concurrent readers exceeded");
    }

    // Must be called after the slab is unlinked from all reader-visible indices.
    void retire(FixedSlabPool& pool, GraphNodeSlab* node) {
        if (node == nullptr) {
            return;
        }
        std::lock_guard lock(advance_mutex_);
        const std::uint64_t epoch = global_epoch_.load(std::memory_order_relaxed);
        pool.retire_slot(node, epoch);
    }

    // Advances the global epoch if all active readers have observed the current
    // epoch, reclaiming slots retired two epochs ago (`new_epoch - 2`).
    template <typename OnReclaim>
    std::size_t try_advance(FixedSlabPool& pool, OnReclaim&& on_reclaim) {
        std::lock_guard lock(advance_mutex_);
        const std::uint64_t cur = global_epoch_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (const auto& reader : readers_) {
            if (reader.epoch.load(std::memory_order_acquire) < cur) {
                return 0;
            }
        }
        global_epoch_.store(cur + 1, std::memory_order_release);
        if (cur >= 1) {
            return pool.reclaim_epoch(cur - 1, std::forward<OnReclaim>(on_reclaim));
        }
        return 0;
    }

    std::size_t try_advance(FixedSlabPool& pool) {
        return try_advance(pool, [](const GraphNodeSlab&) noexcept {});
    }

    [[nodiscard]] std::uint64_t current_epoch() const noexcept {
        return global_epoch_.load(std::memory_order_acquire);
    }

private:
    // 128-byte alignment avoids false sharing on both 64B (x86-64) and 128B
    // (Apple Silicon / ARM64) cache-line architectures.
    struct alignas(128) ReaderSlot {
        std::atomic<std::uint64_t> epoch{kInactive};
        std::atomic<bool> in_use{false};
    };

    void enter_read(std::size_t index) noexcept {
        const std::uint64_t e = global_epoch_.load(std::memory_order_relaxed);
        readers_[index].epoch.store(e, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    void exit_read(std::size_t index) noexcept {
        readers_[index].epoch.store(kInactive, std::memory_order_release);
    }

    void unregister_reader(std::size_t index) noexcept {
        readers_[index].epoch.store(kInactive, std::memory_order_release);
        readers_[index].in_use.store(false, std::memory_order_release);
    }

    alignas(128) std::atomic<std::uint64_t> global_epoch_{0};
    std::mutex advance_mutex_;
    std::array<ReaderSlot, kMaxReaders> readers_{};
};

}  // namespace low_latency_graphdb
