#pragma once

#include "low_latency_graphdb/adjacency_view.hpp"
#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/storage/rocksdb_graph_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace low_latency_graphdb::storage {

// Live, read-through in-memory hot tier backed by `FixedSlabPool` + EBR
// (`EpochTracker`).
//
// Vertices are loaded on demand from RocksDB on first access — there is no
// separate warm() / rebuild step. Each tracked vertex owns a stable 32-bit
// `LocalNodeId` in [0, capacity) mapped to an atomic `GraphNodeSlab*` allocated
// from `FixedSlabPool`. Writes update loaded slabs via copy-on-write (allocate
// fresh 64-byte slab, atomic exchange, retire old slab through EBR) so hot
// vertices stay hot immediately without invalidating or rebuilding the tier.
class HotTier {
public:
    static constexpr LocalNodeId kNotFound = std::numeric_limits<LocalNodeId>::max();
    static constexpr std::uint32_t kLoadedFlag = 1;
    // Default capacity: 1,048,576 vertices (~64 MiB of slabs + directory).
    static constexpr std::size_t kDefaultCapacity = 1U << 20;

    struct Stats {
        std::size_t loaded_nodes{0};
        std::size_t registered_nodes{0};
        std::size_t pool_capacity{0};
        std::size_t pool_available{0};
        std::size_t retired_slots{0};
        std::size_t overflow_edges{0};
        std::size_t bytes{0};
    };

    explicit HotTier(std::size_t capacity = kDefaultCapacity)
        : capacity_(capacity),
          pool_(capacity + std::max<std::size_t>(capacity / 8, 64)),
          epoch_tracker_(std::make_shared<EpochTracker>()) {
        if (capacity_ == 0 || capacity_ >= kNotFound - 1) {
            throw StorageError("hot tier: invalid capacity");
        }
        slabs_ = std::make_unique<std::atomic<const GraphNodeSlab*>[]>(capacity_);
        for (std::size_t i = 0; i < capacity_; ++i) {
            slabs_[i].store(nullptr, std::memory_order_relaxed);
        }
        node_ids_ = std::make_unique<NodeId[]>(capacity_);

        map_capacity_ = 16;
        while (map_capacity_ < capacity_ * 2) {
            map_capacity_ <<= 1;
        }
        map_ = std::make_unique<MapEntry[]>(map_capacity_);
        shift_ = 64;
        for (std::size_t c = map_capacity_; c > 1; c >>= 1) {
            --shift_;
        }
    }

    HotTier(const HotTier&) = delete;
    HotTier& operator=(const HotTier&) = delete;

    ~HotTier() {
        // Reclaim any pending EBR buckets before releasing overflow segments.
        for (std::size_t b = 0; b < FixedSlabPool::kEpochBuckets; ++b) {
            pool_.reclaim_epoch(b, [&](const GraphNodeSlab& node) noexcept {
                if (node.has_overflow()) {
                    overflow_.free(node.overflow_index);
                }
            });
        }
        overflow_.destroy_all();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] EpochTracker& epoch_tracker() const noexcept { return *epoch_tracker_; }
    [[nodiscard]] std::uint64_t write_seq() const noexcept {
        return write_seq_.load(std::memory_order_acquire);
    }

    // Lock-free lookup of an already-registered vertex.
    [[nodiscard]] LocalNodeId find(NodeId id) const noexcept {
        const std::size_t mask = map_capacity_ - 1;
        for (std::size_t i = slot_of(id);; i = (i + 1) & mask) {
            const LocalNodeId local = map_[i].local.load(std::memory_order_acquire);
            if (local == kNotFound) {
                return kNotFound;
            }
            if (map_[i].key == id) {
                return local;
            }
        }
    }

    // Returns the vertex's dense LocalNodeId, assigning one if capacity remains.
    // Returns kNotFound if the vertex is unknown and capacity is exhausted.
    [[nodiscard]] LocalNodeId ensure_local(NodeId id) {
        if (const LocalNodeId existing = find(id); existing != kNotFound) {
            return existing;
        }
        std::lock_guard lock(meta_mutex_);
        return ensure_local_locked(id);
    }

    [[nodiscard]] NodeId node_id(LocalNodeId local) const noexcept {
        return node_ids_[local];
    }

    // Returns the live 64B slab for `local`, or nullptr if not yet loaded.
    // Caller must hold an `EpochTracker::ReadGuard` while dereferencing it.
    [[nodiscard]] const GraphNodeSlab* slab_ptr(LocalNodeId local) const noexcept {
        return slabs_[local].load(std::memory_order_acquire);
    }

    template <typename Visitor>
    void for_each_neighbor(const GraphNodeSlab* node, Visitor&& visitor) const {
        const std::uint32_t inline_count =
            std::min<std::uint32_t>(node->count, node->inline_neighbors.size());
        for (std::uint32_t i = 0; i < inline_count; ++i) {
            visitor(node->inline_neighbors[i]);
        }
        if (node->has_overflow()) {
            const LocalNodeId* extra = overflow_.get(node->overflow_index);
            const std::uint32_t extra_count = node->count - node->inline_neighbors.size();
            for (std::uint32_t i = 0; i < extra_count; ++i) {
                visitor(extra[i]);
            }
        }
    }

    // On-demand read-through population. Publishes a newly allocated slab for
    // `local` only if no slab is loaded yet and no concurrent write invalidated
    // `expected_write_seq`. Returns the live slab pointer, or nullptr if the
    // row could not be cached (concurrent write or pool/ID capacity reached).
    const GraphNodeSlab* populate_if_missing(LocalNodeId local, ZeroCopyAdjView view,
                                             std::uint64_t expected_write_seq) {
        std::lock_guard lock(meta_mutex_);
        if (const GraphNodeSlab* existing = slabs_[local].load(std::memory_order_relaxed)) {
            return existing;
        }
        if (write_seq_.load(std::memory_order_relaxed) != expected_write_seq) {
            return nullptr;
        }
        if (view.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw StorageError("hot tier: degree exceeds 32 bits");
        }
        scratch_neighbors_.resize(view.size());
        for (std::size_t i = 0; i < view.size(); ++i) {
            const LocalNodeId nl = ensure_local_locked(view.target(i));
            if (nl == kNotFound) {
                return nullptr;
            }
            scratch_neighbors_[i] = nl;
        }
        GraphNodeSlab* slab = build_slab_locked(node_ids_[local], scratch_neighbors_);
        if (slab == nullptr) {
            return nullptr;
        }
        slabs_[local].store(slab, std::memory_order_release);
        ++loaded_nodes_;
        return slab;
    }

    // Live COW update after a RocksDB commit. If `src` is currently cached,
    // swaps in a fresh 64B slab from `FixedSlabPool` and retires the old slab
    // through EBR. Falls back to unlinking (retiring the old slab) if capacity
    // is exhausted so stale data is never served.
    void update_if_present(NodeId src, std::span<const NodeId> targets) {
        std::lock_guard lock(meta_mutex_);
        write_seq_.fetch_add(1, std::memory_order_acq_rel);

        const LocalNodeId self = find(src);
        if (self == kNotFound) {
            return;
        }
        const GraphNodeSlab* current = slabs_[self].load(std::memory_order_relaxed);
        if (current == nullptr) {
            return;
        }

        GraphNodeSlab* replacement = nullptr;
        if (targets.size() <= std::numeric_limits<std::uint32_t>::max()) {
            scratch_neighbors_.resize(targets.size());
            bool all_mapped = true;
            for (std::size_t i = 0; i < targets.size(); ++i) {
                const LocalNodeId nl = ensure_local_locked(targets[i]);
                if (nl == kNotFound) {
                    all_mapped = false;
                    break;
                }
                scratch_neighbors_[i] = nl;
            }
            if (all_mapped) {
                replacement = build_slab_locked(src, scratch_neighbors_);
            }
        }

        const GraphNodeSlab* old = slabs_[self].exchange(replacement, std::memory_order_acq_rel);
        if (replacement == nullptr && old != nullptr) {
            --loaded_nodes_;
        }
        if (old != nullptr) {
            epoch_tracker_->retire(pool_, const_cast<GraphNodeSlab*>(old));
            reclaim_locked();
        }
    }

    std::size_t try_reclaim() {
        std::lock_guard lock(meta_mutex_);
        return reclaim_locked();
    }

    [[nodiscard]] Stats stats() const {
        std::lock_guard lock(meta_mutex_);
        Stats s;
        s.loaded_nodes = loaded_nodes_;
        s.registered_nodes = next_local_;
        s.pool_capacity = pool_.capacity();
        s.pool_available = pool_.available();
        s.retired_slots = pool_.retired_count();
        s.overflow_edges = overflow_edges_;
        s.bytes = pool_.capacity() * sizeof(SlabSlot) +
                  capacity_ * (sizeof(std::atomic<const GraphNodeSlab*>) + sizeof(NodeId)) +
                  map_capacity_ * sizeof(MapEntry) +
                  overflow_edges_ * sizeof(LocalNodeId);
        return s;
    }

private:
    struct MapEntry {
        NodeId key{0};
        std::atomic<LocalNodeId> local{kNotFound};
    };

    // Segmented table of heap-allocated overflow neighbor arrays, addressed by
    // 32-bit `overflow_index` stored in `GraphNodeSlab`. Readers perform a
    // single lock-free load; freed arrays are recycled only after EBR reclaim.
    class OverflowStore {
    public:
        static constexpr std::size_t kSegShift = 10;
        static constexpr std::size_t kSegSize = 1U << kSegShift;
        static constexpr std::size_t kSegMask = kSegSize - 1;
        static constexpr std::size_t kMaxSegs = 1024;  // up to 1,048,576 overflow arrays

        std::uint32_t allocate(std::span<const LocalNodeId> extra) {
            auto* arr = new LocalNodeId[extra.size()];
            std::copy(extra.begin(), extra.end(), arr);
            std::uint32_t id = 0;
            if (!free_ids_.empty()) {
                id = free_ids_.back();
                free_ids_.pop_back();
            } else {
                const std::size_t seg = next_id_ >> kSegShift;
                if (seg >= kMaxSegs) {
                    delete[] arr;
                    throw StorageError("hot tier: overflow table exhausted");
                }
                if (segments_[seg].load(std::memory_order_relaxed) == nullptr) {
                    auto* block = new std::atomic<LocalNodeId*>[kSegSize];
                    for (std::size_t i = 0; i < kSegSize; ++i) {
                        block[i].store(nullptr, std::memory_order_relaxed);
                    }
                    segments_[seg].store(block, std::memory_order_release);
                }
                id = next_id_++;
            }
            auto* block = segments_[id >> kSegShift].load(std::memory_order_relaxed);
            block[id & kSegMask].store(arr, std::memory_order_release);
            return id;
        }

        [[nodiscard]] const LocalNodeId* get(std::uint32_t id) const noexcept {
            const auto* block = segments_[id >> kSegShift].load(std::memory_order_acquire);
            return block[id & kSegMask].load(std::memory_order_acquire);
        }

        void free(std::uint32_t id) noexcept {
            auto* block = segments_[id >> kSegShift].load(std::memory_order_relaxed);
            LocalNodeId* arr = block[id & kSegMask].exchange(nullptr, std::memory_order_relaxed);
            delete[] arr;
            free_ids_.push_back(id);
        }

        void destroy_all() noexcept {
            for (std::size_t s = 0; s < kMaxSegs; ++s) {
                auto* block = segments_[s].exchange(nullptr, std::memory_order_relaxed);
                if (block == nullptr) {
                    continue;
                }
                for (std::size_t i = 0; i < kSegSize; ++i) {
                    delete[] block[i].load(std::memory_order_relaxed);
                }
                delete[] block;
            }
            free_ids_.clear();
            next_id_ = 0;
        }

    private:
        std::array<std::atomic<std::atomic<LocalNodeId*>*>, kMaxSegs> segments_{};
        std::vector<std::uint32_t> free_ids_;
        std::uint32_t next_id_{0};
    };

    std::size_t slot_of(NodeId id) const noexcept {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    LocalNodeId ensure_local_locked(NodeId id) {
        if (const LocalNodeId existing = find(id); existing != kNotFound) {
            return existing;
        }
        if (next_local_ >= capacity_) {
            return kNotFound;
        }
        const LocalNodeId local = next_local_++;
        node_ids_[local] = id;
        const std::size_t mask = map_capacity_ - 1;
        std::size_t i = slot_of(id);
        while (map_[i].local.load(std::memory_order_relaxed) != kNotFound) {
            i = (i + 1) & mask;
        }
        map_[i].key = id;
        map_[i].local.store(local, std::memory_order_release);
        return local;
    }

    GraphNodeSlab* build_slab_locked(NodeId id, std::span<const LocalNodeId> neighbors) {
        GraphNodeSlab* slab = pool_.allocate();
        if (slab == nullptr) {
            reclaim_locked();
            slab = pool_.allocate();
            if (slab == nullptr) {
                return nullptr;
            }
        }
        slab->node_id = id;
        slab->count = static_cast<std::uint32_t>(neighbors.size());
        slab->flags = kLoadedFlag;
        const std::size_t inline_count =
            std::min(neighbors.size(), slab->inline_neighbors.size());
        std::copy_n(neighbors.begin(), inline_count, slab->inline_neighbors.begin());
        if (neighbors.size() > inline_count) {
            const auto extra = neighbors.subspan(inline_count);
            slab->overflow_index = overflow_.allocate(extra);
            overflow_edges_ += extra.size();
        }
        return slab;
    }

    std::size_t reclaim_locked() noexcept {
        return epoch_tracker_->try_advance(pool_, [&](const GraphNodeSlab& node) noexcept {
            if (node.has_overflow()) {
                overflow_.free(node.overflow_index);
                const std::size_t extra = node.count - node.inline_neighbors.size();
                if (overflow_edges_ >= extra) {
                    overflow_edges_ -= extra;
                }
            }
        });
    }

    std::size_t capacity_{0};
    FixedSlabPool pool_;
    std::shared_ptr<EpochTracker> epoch_tracker_;
    std::unique_ptr<std::atomic<const GraphNodeSlab*>[]> slabs_;
    std::unique_ptr<NodeId[]> node_ids_;
    std::size_t map_capacity_{0};
    std::unique_ptr<MapEntry[]> map_;
    std::uint32_t shift_{64};
    OverflowStore overflow_;
    std::atomic<std::uint64_t> write_seq_{0};

    mutable std::mutex meta_mutex_;
    LocalNodeId next_local_{0};
    std::size_t loaded_nodes_{0};
    std::size_t overflow_edges_{0};
    std::vector<LocalNodeId> scratch_neighbors_;
};

}  // namespace low_latency_graphdb::storage
