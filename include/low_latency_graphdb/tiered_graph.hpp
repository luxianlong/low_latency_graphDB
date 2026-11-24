#pragma once

#include "low_latency_graphdb/generation_set.hpp"
#include "low_latency_graphdb/hot_tier.hpp"
#include "low_latency_graphdb/query_arena.hpp"
#include "low_latency_graphdb/storage_bfs.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace low_latency_graphdb::storage {

struct TieredBfsStats {
    std::size_t visited{0};
    std::size_t hot_rows{0};    // expanded from the in-memory FixedSlabPool
    std::size_t cold_rows{0};   // loaded on demand from RocksDB
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
    explicit TieredBfsState(std::size_t arena_capacity = QueryArena::kDefaultCapacity)
        : arena_capacity_(arena_capacity) {}

    void reserve(std::size_t expected_visited) {
        seen_dynamic.reserve(expected_visited / 4 + 16);
    }

private:
    friend class TieredGraph;
    const TieredGraph* owner{nullptr};
    EpochTracker::ReaderHandle reader_handle;
    DenseStampSet seen_local;    // keyed by hot-tier LocalNodeId
    GenerationSet seen_dynamic;  // fallback for vertices beyond HotTier capacity
    // Per-thread arena providing the one-direction raw frontier queue;
    // reset after every query (design doc 01).
    std::size_t arena_capacity_{QueryArena::kDefaultCapacity};
    std::unique_ptr<QueryArena> arena;
    // Reusable scratch for on-demand cache misses (untouched on steady-state hits).
    std::vector<std::pair<NodeId, LocalNodeId>> miss_locals;
    std::vector<NodeId> miss_ids;
    std::vector<NodeId> cold_cur, cold_next;
    RocksDbGraphStore::AdjacencyBatch batch;
    ZeroCopyAdjView view;
};

// RocksDB store fronted by an on-demand, read-through `HotTier` backed by
// `FixedSlabPool` + EBR. No warm() / rebuild step is needed: first touch loads
// adjacency rows via pinned RocksDB MultiGet into `FixedSlabPool`, subsequent
// queries hit 100% in memory, and writes update cached slabs via COW + EBR.
//
// Thread-safe: any number of concurrent run() and write_adjacency() calls,
// with one TieredBfsState per querying thread.
class TieredGraph {
public:
    explicit TieredGraph(RocksDbGraphStore& store,
                         std::size_t hot_capacity = HotTier::kDefaultCapacity)
        : store_(store), tier_(hot_capacity) {}

    [[nodiscard]] RocksDbGraphStore& store() noexcept { return store_; }
    [[nodiscard]] const HotTier& hot_tier() const noexcept { return tier_; }
    [[nodiscard]] HotTier& hot_tier() noexcept { return tier_; }

    void write_adjacency(NodeId src, std::span<const std::uint64_t> targets,
                         std::span<const EdgeLightMeta> metas,
                         std::span<const std::string> edge_payloads = {}) {
        std::lock_guard lock(write_mutex_);
        store_.write_adjacency(src, targets, metas, edge_payloads);
        tier_.update_if_present(src, targets);
    }

    // Visits the subgraph within `max_depth` hops. When `out` is non-null the
    // visited IDs are appended (level by level, unspecified order within a
    // level); otherwise only the count is produced. Returns the visited count.
    std::size_t run(NodeId start, const TieredBfsOptions& options, TieredBfsState& state,
                    std::vector<NodeId>* out = nullptr,
                    TieredBfsStats* stats = nullptr) const {
        if (state.owner != this) {
            state.reader_handle = tier_.epoch_tracker().register_reader();
            state.seen_local.resize(tier_.capacity());
            state.owner = this;
        }
        EpochTracker::ReadGuard epoch_guard(tier_.epoch_tracker(), state.reader_handle);

        const std::size_t chunk = std::clamp<std::size_t>(
            options.pin_batch_size, 1, RocksDbGraphStore::kMaxPinBatchSize);

        state.seen_local.clear();
        state.seen_dynamic.clear();
        state.cold_cur.clear();
        state.cold_next.clear();

        // Claim the per-thread arena directly as a one-direction raw frontier
        // queue of LocalNodeIds — no std::vector, no dual-ended pointers, O(1) reset.
        if (!state.arena) {
            state.arena = std::make_unique<QueryArena>(state.arena_capacity_);
        }
        ArenaScope arena_scope(*state.arena);
        const std::size_t usable_bytes =
            state.arena->capacity() & ~std::size_t{alignof(LocalNodeId) - 1};
        if (usable_bytes < sizeof(LocalNodeId)) {
            throw_arena_oom();
        }
        auto* raw = static_cast<char*>(
            state.arena->allocate(usable_bytes, QueryArena::kAlignment));
        auto* hot_cur_begin = reinterpret_cast<LocalNodeId*>(raw);
        auto* hot_cur_end = hot_cur_begin;
        auto* hot_tail = hot_cur_begin;
        const LocalNodeId* const hot_limit =
            hot_cur_begin + (usable_bytes / sizeof(LocalNodeId));

        TieredBfsStats local;
        std::optional<RocksDbGraphStore::Snapshot> snapshot;
        std::uint64_t snapshot_seq = 0;

        const LocalNodeId start_local = tier_.ensure_local(start);
        if (start_local != HotTier::kNotFound) {
            state.seen_local.insert(start_local);
            push_hot(hot_tail, hot_limit, start_local);
        } else {
            state.seen_dynamic.insert(start);
            state.cold_cur.push_back(start);
        }
        hot_cur_end = hot_tail;

        std::size_t visited = 0;
        for (std::size_t depth = 0;; ++depth) {
            const std::size_t hot_count =
                static_cast<std::size_t>(hot_cur_end - hot_cur_begin);
            const std::size_t cold_count = state.cold_cur.size();
            const std::size_t level = hot_count + cold_count;
            if (level == 0) {
                break;
            }
            if (visited + level > options.max_visited) {
                throw StorageError("tiered BFS exceeds max_visited");
            }
            visited += level;
            if (out != nullptr) {
                for (const LocalNodeId* p = hot_cur_begin; p != hot_cur_end; ++p) {
                    out->push_back(tier_.node_id(*p));
                }
                if (cold_count != 0) {
                    out->insert(out->end(), state.cold_cur.begin(), state.cold_cur.end());
                }
            }
            if (depth >= options.max_depth) {
                break;
            }

            // Neighbors discovered on the last expansion are only counted when
            // no ID output is requested, which avoids touching their slabs.
            const bool count_only_leaves = out == nullptr && depth + 1 >= options.max_depth;
            std::size_t leaves = 0;
            state.miss_locals.clear();
            state.cold_next.clear();

            if (count_only_leaves) {
                for (const LocalNodeId* p = hot_cur_begin; p != hot_cur_end; ++p) {
                    const LocalNodeId cur = *p;
                    const GraphNodeSlab* node = tier_.slab_ptr(cur);
                    if (node == nullptr) [[unlikely]] {
                        state.miss_locals.emplace_back(tier_.node_id(cur), cur);
                        continue;
                    }
                    ++local.hot_rows;
                    tier_.for_each_neighbor(node, [&](LocalNodeId nl) {
                        if (state.seen_local.insert(nl)) {
                            ++leaves;
                        }
                    });
                }
            } else {
                for (const LocalNodeId* p = hot_cur_begin; p != hot_cur_end; ++p) {
                    const LocalNodeId cur = *p;
                    const GraphNodeSlab* node = tier_.slab_ptr(cur);
                    if (node == nullptr) [[unlikely]] {
                        state.miss_locals.emplace_back(tier_.node_id(cur), cur);
                        continue;
                    }
                    ++local.hot_rows;
                    tier_.for_each_neighbor(node, [&](LocalNodeId nl) {
                        if (state.seen_local.insert(nl)) {
                            push_hot(hot_tail, hot_limit, nl);
                        }
                    });
                }
            }

            // On-demand read-through for vertices not yet loaded in FixedSlabPool.
            if (!state.miss_locals.empty()) [[unlikely]] {
                std::sort(state.miss_locals.begin(), state.miss_locals.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                if (!snapshot) {
                    snapshot_seq = tier_.write_seq();
                    snapshot.emplace(store_.snapshot());
                }
                const std::size_t miss_n = state.miss_locals.size();
                state.miss_ids.resize(miss_n);
                for (std::size_t i = 0; i < miss_n; ++i) {
                    state.miss_ids[i] = state.miss_locals[i].first;
                }
                for (std::size_t offset = 0; offset < miss_n; offset += chunk) {
                    const std::size_t count = std::min(chunk, miss_n - offset);
                    store_.read_adjacency_batch(
                        std::span<const NodeId>(state.miss_ids.data() + offset, count),
                        state.batch, &*snapshot);
                    for (std::size_t i = 0; i < state.batch.size(); ++i) {
                        if (!state.batch.found(i)) {
                            ++local.cold_missing;
                            continue;
                        }
                        ++local.cold_rows;
                        state.batch.bind(i, state.view);
                        const LocalNodeId cur_local = state.miss_locals[offset + i].second;
                        const GraphNodeSlab* loaded =
                            tier_.populate_if_missing(cur_local, state.view, snapshot_seq);
                        if (loaded != nullptr) {
                            tier_.for_each_neighbor(loaded, [&](LocalNodeId nl) {
                                if (!state.seen_local.insert(nl)) {
                                    return;
                                }
                                if (count_only_leaves) {
                                    ++leaves;
                                } else {
                                    push_hot(hot_tail, hot_limit, nl);
                                }
                            });
                        } else {
                            expand_view(state.view, count_only_leaves, leaves, hot_tail,
                                        hot_limit, state);
                        }
                    }
                    state.batch.clear();
                }
            }

            // Fallback path only if HotTier capacity was exhausted.
            if (cold_count != 0) [[unlikely]] {
                std::sort(state.cold_cur.begin(), state.cold_cur.end());
                if (!snapshot) {
                    snapshot.emplace(store_.snapshot());
                }
                for (std::size_t offset = 0; offset < cold_count; offset += chunk) {
                    const std::size_t count = std::min(chunk, cold_count - offset);
                    store_.read_adjacency_batch(
                        std::span<const NodeId>(state.cold_cur.data() + offset, count),
                        state.batch, &*snapshot);
                    for (std::size_t i = 0; i < state.batch.size(); ++i) {
                        if (!state.batch.found(i)) {
                            ++local.cold_missing;
                            continue;
                        }
                        ++local.cold_rows;
                        state.batch.bind(i, state.view);
                        expand_view(state.view, count_only_leaves, leaves, hot_tail, hot_limit,
                                    state);
                    }
                    state.batch.clear();
                }
            }

            if (count_only_leaves) {
                if (visited + leaves > options.max_visited) {
                    throw StorageError("tiered BFS exceeds max_visited");
                }
                visited += leaves;
                break;
            }
            hot_cur_begin = hot_cur_end;
            hot_cur_end = hot_tail;
            state.cold_cur.swap(state.cold_next);
        }

        local.visited = visited;
        if (stats != nullptr) {
            *stats = local;
        }
        return visited;
    }

private:
    void expand_view(const ZeroCopyAdjView& view, bool count_only_leaves, std::size_t& leaves,
                     LocalNodeId*& hot_tail, const LocalNodeId* hot_limit,
                     TieredBfsState& state) const {
        for (std::size_t j = 0; j < view.size(); ++j) {
            const NodeId target = view.target(j);
            const LocalNodeId nl = tier_.ensure_local(target);
            if (nl != HotTier::kNotFound) {
                if (!state.seen_local.insert(nl)) {
                    continue;
                }
                if (count_only_leaves) {
                    ++leaves;
                } else {
                    push_hot(hot_tail, hot_limit, nl);
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

    static void push_hot(LocalNodeId*& hot_tail, const LocalNodeId* hot_limit, LocalNodeId nl) {
        if (hot_tail == hot_limit) [[unlikely]] {
            throw_arena_oom();
        }
        *hot_tail++ = nl;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline))
#endif
    [[noreturn]] static void throw_arena_oom() {
        throw std::runtime_error("STATUS_QUERY_MEMORY_LIMIT_EXCEEDED");
    }

    RocksDbGraphStore& store_;
    mutable HotTier tier_;
    std::mutex write_mutex_;
};

}  // namespace low_latency_graphdb::storage
