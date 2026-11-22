#pragma once

#include "low_latency_graphdb/generation_set.hpp"
#include "low_latency_graphdb/hot_tier.hpp"
#include "low_latency_graphdb/storage_bfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <optional>
#include <span>
#include <vector>

namespace low_latency_graphdb::storage {

struct TieredBfsStats {
    std::size_t visited{0};
    std::size_t hot_rows{0};    // expanded from the in-memory hot tier
    std::size_t cold_rows{0};   // expanded from RocksDB
    std::size_t cold_missing{0};
};

struct TieredBfsOptions {
    std::size_t max_depth{1};
    std::size_t max_visited{1'000'000};
    std::size_t pin_batch_size{1024};
};

class TieredGraph;

// Per-thread reusable query state; steady-state queries do not allocate.
class TieredBfsState {
public:
    void reserve(std::size_t expected_visited) {
        seen_dynamic.reserve(expected_visited / 4 + 16);
    }

private:
    friend class TieredGraph;
    // Cached tier: refreshed only when the graph publishes a new version, so
    // concurrent queries never contend on a shared lock or refcount.
    const TieredGraph* owner{nullptr};
    std::uint64_t tier_version{0};
    std::shared_ptr<const HotTier> tier;
    DenseStampSet seen_local;    // keyed by hot-tier local ID
    GenerationSet seen_dynamic;  // keyed by NodeId, for vertices unknown to the tier
    std::vector<LocalNodeId> hot_cur, hot_next;
    std::vector<NodeId> cold_cur, cold_next;
    RocksDbGraphStore::AdjacencyBatch batch;
    ZeroCopyAdjView view;
};

// RocksDB store fronted by an in-memory hot tier. Reads of hot vertices never
// touch RocksDB; everything else goes through pinned MultiGet batches. Writes
// go to RocksDB first and then invalidate the hot copy, so queries always see
// committed data (a hot vertex rewritten mid-query may still be served from
// the pre-write copy until its flag is set).
//
// Thread-safe: any number of concurrent run()/write_*()/refresh calls, with one
// TieredBfsState per querying thread.
class TieredGraph {
public:
    explicit TieredGraph(RocksDbGraphStore& store)
        : store_(store), tier_(HotTier::build(store, {})) {}

    [[nodiscard]] RocksDbGraphStore& store() noexcept { return store_; }

    // Rebuild the hot tier from the highest-degree vertices / explicit IDs.
    // Writers are paused during the build so no committed write can be missed.
    void warm_top_degree(std::size_t limit) {
        std::unique_lock lock(write_mutex_);
        publish(HotTier::build_top_degree(store_, limit));
    }
    void warm(std::span<const NodeId> ids) {
        std::unique_lock lock(write_mutex_);
        publish(HotTier::build(store_, ids));
    }

    [[nodiscard]] std::shared_ptr<const HotTier> hot_tier() const {
        std::lock_guard lock(tier_mutex_);
        return tier_;
    }

    void write_adjacency(NodeId src, std::span<const std::uint64_t> targets,
                         std::span<const EdgeLightMeta> metas,
                         std::span<const std::string> edge_payloads = {}) {
        std::shared_lock lock(write_mutex_);
        store_.write_adjacency(src, targets, metas, edge_payloads);
        hot_tier()->invalidate(src);
    }

    // Visits the subgraph within `max_depth` hops. When `out` is non-null the
    // visited IDs are appended (level by level, unspecified order within a
    // level); otherwise only the count is produced. Returns the visited count.
    std::size_t run(NodeId start, const TieredBfsOptions& options, TieredBfsState& state,
                    std::vector<NodeId>* out = nullptr,
                    TieredBfsStats* stats = nullptr) const {
        const std::uint64_t version = tier_version_.load(std::memory_order_acquire);
        if (state.owner != this || state.tier_version != version || !state.tier) {
            std::lock_guard lock(tier_mutex_);
            state.tier = tier_;
            state.tier_version = tier_version_.load(std::memory_order_relaxed);
            state.seen_local.resize(state.tier->size());
            state.owner = this;
        }
        const HotTier& tier = *state.tier;
        const std::size_t chunk = std::clamp<std::size_t>(
            options.pin_batch_size, 1, RocksDbGraphStore::kMaxPinBatchSize);

        state.seen_local.clear();
        state.seen_dynamic.clear();
        state.hot_cur.clear();
        state.hot_next.clear();
        state.cold_cur.clear();
        state.cold_next.clear();

        TieredBfsStats local;
        std::optional<RocksDbGraphStore::Snapshot> snapshot;

        // Places a vertex (already marked seen) into the next frontier.
        auto enqueue_local = [&](LocalNodeId nl, std::vector<LocalNodeId>& hot,
                                 std::vector<NodeId>& cold) {
            if (tier.is_servable(nl)) {
                hot.push_back(nl);
            } else {
                cold.push_back(tier.slab(nl).node_id);
            }
        };

        const LocalNodeId start_local = tier.find(start);
        if (start_local != HotTier::kNotFound) {
            state.seen_local.insert(start_local);
            enqueue_local(start_local, state.hot_cur, state.cold_cur);
        } else {
            state.seen_dynamic.insert(start);
            state.cold_cur.push_back(start);
        }

        std::size_t visited = 0;
        for (std::size_t depth = 0;; ++depth) {
            auto& hot_cur = state.hot_cur;
            auto& cold_cur = state.cold_cur;
            const std::size_t level = hot_cur.size() + cold_cur.size();
            if (level == 0) {
                break;
            }
            if (visited + level > options.max_visited) {
                throw StorageError("tiered BFS exceeds max_visited");
            }
            visited += level;
            if (out != nullptr) {
                for (const LocalNodeId l : hot_cur) out->push_back(tier.slab(l).node_id);
                out->insert(out->end(), cold_cur.begin(), cold_cur.end());
            }
            if (depth >= options.max_depth) {
                break;
            }

            // Neighbors discovered on the last expansion are only counted when
            // no ID output is requested, which avoids touching their slabs.
            const bool count_only_leaves = out == nullptr && depth + 1 >= options.max_depth;
            std::size_t leaves = 0;
            state.hot_next.clear();
            state.cold_next.clear();

            for (const LocalNodeId l : hot_cur) {
                ++local.hot_rows;
                tier.for_each_neighbor(l, [&](LocalNodeId nl) {
                    if (!state.seen_local.insert(nl)) {
                        return;
                    }
                    if (count_only_leaves) {
                        ++leaves;
                    } else {
                        enqueue_local(nl, state.hot_next, state.cold_next);
                    }
                });
            }

            std::sort(cold_cur.begin(), cold_cur.end());
            for (std::size_t offset = 0; offset < cold_cur.size(); offset += chunk) {
                const std::size_t count = std::min(chunk, cold_cur.size() - offset);
                if (!snapshot) {
                    snapshot.emplace(store_.snapshot());
                }
                store_.read_adjacency_batch(
                    std::span<const NodeId>(cold_cur.data() + offset, count), state.batch,
                    &*snapshot);
                for (std::size_t i = 0; i < state.batch.size(); ++i) {
                    if (!state.batch.found(i)) {
                        ++local.cold_missing;
                        continue;
                    }
                    ++local.cold_rows;
                    state.batch.bind(i, state.view);
                    for (std::size_t j = 0; j < state.view.size(); ++j) {
                        const NodeId target = state.view.target(j);
                        const LocalNodeId nl = tier.find(target);
                        if (nl != HotTier::kNotFound) {
                            if (!state.seen_local.insert(nl)) {
                                continue;
                            }
                            if (count_only_leaves) {
                                ++leaves;
                            } else {
                                enqueue_local(nl, state.hot_next, state.cold_next);
                            }
                        } else if (state.seen_dynamic.insert(target)) {
                            if (count_only_leaves) {
                                ++leaves;
                            } else {
                                state.cold_next.push_back(target);
                            }
                        }
                    }
                }
                state.batch.clear();
            }

            if (count_only_leaves) {
                if (visited + leaves > options.max_visited) {
                    throw StorageError("tiered BFS exceeds max_visited");
                }
                visited += leaves;
                break;
            }
            hot_cur.swap(state.hot_next);
            cold_cur.swap(state.cold_next);
        }

        local.visited = visited;
        if (stats != nullptr) {
            *stats = local;
        }
        return visited;
    }

private:
    void publish(std::shared_ptr<const HotTier> tier) {
        std::lock_guard lock(tier_mutex_);
        tier_ = std::move(tier);
        tier_version_.fetch_add(1, std::memory_order_release);
    }

    RocksDbGraphStore& store_;
    mutable std::mutex tier_mutex_;
    std::shared_mutex write_mutex_;
    std::atomic<std::uint64_t> tier_version_{1};
    std::shared_ptr<const HotTier> tier_;
};

}  // namespace low_latency_graphdb::storage
