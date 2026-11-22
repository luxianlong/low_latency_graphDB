#pragma once

#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/rocksdb_graph_store.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <queue>
#include <span>
#include <unordered_map>
#include <vector>

namespace low_latency_graphdb::storage {

// Immutable in-memory snapshot of the hottest vertices (Tier 1 of the design).
//
// Every hot vertex owns a 64-byte GraphNodeSlab whose neighbors are local IDs.
// Neighbors that are not themselves hot get a "stub" slab (no adjacency, only
// the node ID) so the traversal can stay in the dense local-ID space and
// hand the stub to RocksDB when it is reached. Staleness is handled with
// per-slab dirty flags: a vertex invalidated after the build is traversed via
// RocksDB, which stays the source of truth, until the tier is rebuilt.
class HotTier {
public:
    static constexpr LocalNodeId kNotFound = std::numeric_limits<LocalNodeId>::max();
    static constexpr std::uint32_t kHotFlag = 1;

    struct Stats {
        std::size_t hot_nodes{0};
        std::size_t stub_nodes{0};
        std::size_t overflow_edges{0};
        std::size_t bytes{0};
    };

    // Rebuilds from an explicit vertex set (missing rows are skipped).
    static std::shared_ptr<const HotTier> build(RocksDbGraphStore& store,
                                                std::span<const NodeId> hot_ids) {
        Builder builder;
        RocksDbGraphStore::AdjacencyHandle handle;
        std::vector<NodeId> ids(hot_ids.begin(), hot_ids.end());
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        for (const NodeId id : ids) {
            builder.reserve_hot(id);
        }
        for (const NodeId id : ids) {
            if (!store.read_adjacency(id, handle)) {
                continue;
            }
            const ZeroCopyAdjView& view = handle.view();
            builder.fill_hot(id, view);
        }
        return builder.finish();
    }

    // Picks the `limit` highest-degree vertices (ties: lower ID wins).
    static std::shared_ptr<const HotTier> build_top_degree(RocksDbGraphStore& store,
                                                           std::size_t limit) {
        using Entry = std::pair<std::uint32_t, NodeId>;  // degree, id
        auto worse = [](const Entry& a, const Entry& b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        };
        std::priority_queue<Entry, std::vector<Entry>, decltype(worse)> heap(worse);
        store.for_each_adjacency([&](NodeId id, const ZeroCopyAdjView& view) {
            const Entry entry{static_cast<std::uint32_t>(view.size()), id};
            if (heap.size() < limit) {
                heap.push(entry);
            } else if (!heap.empty() && worse(heap.top(), entry)) {
                heap.pop();
                heap.push(entry);
            }
        });
        std::vector<NodeId> ids;
        ids.reserve(heap.size());
        for (; !heap.empty(); heap.pop()) {
            ids.push_back(heap.top().second);
        }
        std::sort(ids.begin(), ids.end());
        return build(store, ids);
    }

    [[nodiscard]] LocalNodeId find(NodeId id) const noexcept {
        if (map_.empty()) {
            return kNotFound;
        }
        const std::size_t mask = map_.size() - 1;
        for (std::size_t i = slot_of(id);; i = (i + 1) & mask) {
            const MapEntry& entry = map_[i];
            if (entry.local == kNotFound) {
                return kNotFound;
            }
            if (entry.id == id) {
                return entry.local;
            }
        }
    }

    [[nodiscard]] const GraphNodeSlab& slab(LocalNodeId local) const noexcept {
        return slabs_[local];
    }

    // Hot and not invalidated since the build.
    [[nodiscard]] bool is_servable(LocalNodeId local) const noexcept {
        return (slabs_[local].flags & kHotFlag) != 0 &&
               dirty_[local].load(std::memory_order_relaxed) == 0;
    }

    template <typename Visitor>
    void for_each_neighbor(LocalNodeId local, Visitor&& visitor) const {
        const GraphNodeSlab& node = slabs_[local];
        const std::uint32_t inline_count =
            std::min<std::uint32_t>(node.count, node.inline_neighbors.size());
        for (std::uint32_t i = 0; i < inline_count; ++i) {
            visitor(node.inline_neighbors[i]);
        }
        if (node.has_overflow()) {
            const LocalNodeId* extra = overflow_.data() + node.overflow_index;
            const std::uint32_t extra_count = node.count - node.inline_neighbors.size();
            for (std::uint32_t i = 0; i < extra_count; ++i) {
                visitor(extra[i]);
            }
        }
    }

    // Marks a vertex's cached row stale. Safe to call concurrently with reads.
    void invalidate(NodeId id) const noexcept {
        const LocalNodeId local = find(id);
        if (local != kNotFound && (slabs_[local].flags & kHotFlag) != 0) {
            dirty_[local].store(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return slabs_.size(); }
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    struct MapEntry {
        NodeId id{0};
        LocalNodeId local{kNotFound};
    };

    struct Builder {
        std::vector<GraphNodeSlab> slabs;
        std::vector<LocalNodeId> overflow;
        std::unordered_map<NodeId, LocalNodeId> index;
        std::size_t hot{0};

        LocalNodeId local_for(NodeId id) {
            const auto [it, inserted] =
                index.try_emplace(id, static_cast<LocalNodeId>(slabs.size()));
            if (inserted) {
                if (slabs.size() >= kNotFound - 1) {
                    throw StorageError("hot tier: local ID space exhausted");
                }
                slabs.emplace_back();
                slabs.back().node_id = id;
            }
            return it->second;
        }

        void reserve_hot(NodeId id) { local_for(id); }

        void fill_hot(NodeId id, const ZeroCopyAdjView& view) {
            if (view.size() > std::numeric_limits<std::uint32_t>::max()) {
                throw StorageError("hot tier: degree exceeds 32 bits");
            }
            const LocalNodeId self = local_for(id);
            std::vector<LocalNodeId> neighbors(view.size());
            for (std::size_t i = 0; i < view.size(); ++i) {
                neighbors[i] = local_for(view.target(i));
            }
            GraphNodeSlab& node = slabs[self];  // after all local_for calls
            node.flags |= kHotFlag;
            node.count = static_cast<std::uint32_t>(neighbors.size());
            const std::size_t inline_count =
                std::min(neighbors.size(), node.inline_neighbors.size());
            std::copy_n(neighbors.begin(), inline_count, node.inline_neighbors.begin());
            if (neighbors.size() > inline_count) {
                node.overflow_index = static_cast<std::uint32_t>(overflow.size());
                overflow.insert(overflow.end(), neighbors.begin() + inline_count,
                                neighbors.end());
            }
            ++hot;
        }

        std::shared_ptr<const HotTier> finish() {
            auto tier = std::shared_ptr<HotTier>(new HotTier());
            tier->slabs_ = std::move(slabs);
            tier->overflow_ = std::move(overflow);
            const std::size_t n = tier->slabs_.size();
            tier->dirty_ = std::make_unique<std::atomic<std::uint8_t>[]>(n);
            std::size_t capacity = 16;
            while (capacity < n * 2) {
                capacity <<= 1;
            }
            tier->map_.assign(capacity, MapEntry{});
            tier->shift_ = 64;
            for (std::size_t c = capacity; c > 1; c >>= 1) {
                --tier->shift_;
            }
            for (std::size_t local = 0; local < n; ++local) {
                const NodeId id = tier->slabs_[local].node_id;
                const std::size_t mask = capacity - 1;
                std::size_t i = tier->slot_of(id);
                while (tier->map_[i].local != kNotFound) {
                    i = (i + 1) & mask;
                }
                tier->map_[i] = {id, static_cast<LocalNodeId>(local)};
            }
            tier->stats_.hot_nodes = hot;
            tier->stats_.stub_nodes = n - hot;
            tier->stats_.overflow_edges = tier->overflow_.size();
            tier->stats_.bytes = n * sizeof(GraphNodeSlab) +
                                 tier->overflow_.size() * sizeof(LocalNodeId) +
                                 capacity * sizeof(MapEntry) + n;
            return tier;
        }
    };

    HotTier() = default;

    std::size_t slot_of(NodeId id) const noexcept {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    std::vector<GraphNodeSlab> slabs_;
    std::vector<LocalNodeId> overflow_;
    std::vector<MapEntry> map_;
    std::unique_ptr<std::atomic<std::uint8_t>[]> dirty_;
    std::uint32_t shift_{64};
    Stats stats_;
};

}  // namespace low_latency_graphdb::storage
