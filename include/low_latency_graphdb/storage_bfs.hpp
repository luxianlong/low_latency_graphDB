#pragma once

#include "low_latency_graphdb/breadth_first_search.hpp"
#include "low_latency_graphdb/rocksdb_graph_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <vector>

namespace low_latency_graphdb::storage {

struct StorageBfsStats {
    std::size_t rows_fetched{0};
    std::size_t rows_missing{0};
    std::size_t rows_pinned{0};   // served zero-copy from a pinned cache page
    std::size_t rows_copied{0};   // served from memtable (RocksDB-owned copy)
    std::size_t aligned_views{0}; // views whose payload allows typed spans
};

struct StorageBfsOptions {
    std::size_t max_depth{1};
    // Fan-out guard: abort with StorageError beyond this many visited vertices.
    std::size_t max_visited{1'000'000};
    // Rows pinned simultaneously per MultiGet; bounded by kMaxPinBatchSize.
    std::size_t pin_batch_size{1024};
    // A snapshot costs a DB-mutex acquisition per query. Disable for
    // read-mostly workloads that tolerate concurrent writes mid-traversal.
    bool use_snapshot{true};
};

// Reusable per-thread query state: generation-stamped open-addressing visited
// set plus frontier/batch buffers, so steady-state queries do not allocate.
class StorageBfsState {
public:
    void begin_query() {
        if (++generation_ == 0) {
            std::fill(slots_.begin(), slots_.end(), Slot{});
            generation_ = 1;
        }
        count_ = 0;
        frontier.clear();
        next.clear();
    }

    // Returns true if `id` was not seen before in this query.
    bool insert(NodeId id) {
        if ((count_ + 1) * 2 > slots_.size()) {
            grow();
        }
        return insert_no_grow(id);
    }

    std::vector<NodeId> frontier;
    std::vector<NodeId> next;
    RocksDbGraphStore::AdjacencyBatch batch;
    ZeroCopyAdjView view;

private:
    struct Slot {
        NodeId id{0};
        std::uint32_t generation{0};
    };

    static std::size_t hash(NodeId id) noexcept {
        id ^= id >> 33;
        id *= 0xff51afd7ed558ccdull;
        id ^= id >> 33;
        return static_cast<std::size_t>(id);
    }

    bool insert_no_grow(NodeId id) {
        const std::size_t mask = slots_.size() - 1;
        for (std::size_t i = hash(id) & mask;; i = (i + 1) & mask) {
            Slot& slot = slots_[i];
            if (slot.generation != generation_) {
                slot.id = id;
                slot.generation = generation_;
                ++count_;
                return true;
            }
            if (slot.id == id) {
                return false;
            }
        }
    }

    void grow() {
        std::vector<Slot> old(slots_.empty() ? 1024 : slots_.size() * 2);
        old.swap(slots_);
        const std::size_t live = count_;
        count_ = 0;
        for (const Slot& slot : old) {
            if (slot.generation == generation_) {
                insert_no_grow(slot.id);
            }
        }
        (void)live;
    }

    std::vector<Slot> slots_;
    std::uint32_t generation_{0};
    std::size_t count_{0};
};

// Level-synchronous BFS reading adjacency straight from RocksDB. Each level is
// deduplicated, sorted, and fetched in pinned MultiGet chunks; pins are
// released as soon as a chunk's neighbors are consumed. Visit order is by
// level, ascending vertex ID within a level.
// Thread-safe: share one instance across threads, but give each thread its own
// StorageBfsState.
class StorageBreadthFirstSearch {
public:
    explicit StorageBreadthFirstSearch(RocksDbGraphStore& store) : store_(store) {}

    [[nodiscard]] TraversalResult run(NodeId start, const StorageBfsOptions& options,
                                      StorageBfsStats* stats = nullptr) const {
        StorageBfsState state;
        return run(start, options, state, stats);
    }

    [[nodiscard]] TraversalResult run(NodeId start, const StorageBfsOptions& options,
                                      StorageBfsState& state,
                                      StorageBfsStats* stats = nullptr) const {
        const std::size_t chunk =
            std::clamp<std::size_t>(options.pin_batch_size, 1,
                                    RocksDbGraphStore::kMaxPinBatchSize);
        std::optional<RocksDbGraphStore::Snapshot> snapshot;
        if (options.use_snapshot) {
            snapshot.emplace(store_.snapshot());
        }
        const RocksDbGraphStore::Snapshot* snapshot_ptr = snapshot ? &*snapshot : nullptr;

        TraversalResult result;
        state.begin_query();
        state.insert(start);
        state.frontier.push_back(start);
        StorageBfsStats local;

        for (std::size_t depth = 0; !state.frontier.empty(); ++depth) {
            auto& frontier = state.frontier;
            std::sort(frontier.begin(), frontier.end());
            if (result.visited.size() + frontier.size() > options.max_visited) {
                throw StorageError("storage BFS exceeds max_visited");
            }
            result.visited.insert(result.visited.end(), frontier.begin(), frontier.end());
            if (depth >= options.max_depth) {
                break;
            }

            state.next.clear();
            for (std::size_t offset = 0; offset < frontier.size(); offset += chunk) {
                const std::size_t count = std::min(chunk, frontier.size() - offset);
                store_.read_adjacency_batch(
                    std::span<const NodeId>(frontier.data() + offset, count), state.batch,
                    snapshot_ptr);
                for (std::size_t i = 0; i < state.batch.size(); ++i) {
                    if (!state.batch.found(i)) {
                        ++local.rows_missing;
                        continue;
                    }
                    ++local.rows_fetched;
                    state.batch.is_pinned(i) ? ++local.rows_pinned : ++local.rows_copied;
                    state.batch.bind(i, state.view);
                    local.aligned_views += state.view.is_zero_copy() ? 1 : 0;
                    for (std::size_t j = 0; j < state.view.size(); ++j) {
                        const NodeId target = state.view.target(j);
                        if (state.insert(target)) {
                            state.next.push_back(target);
                        }
                    }
                }
                state.batch.clear();  // unpin before fetching the next chunk
            }
            frontier.swap(state.next);
        }

        if (stats != nullptr) {
            *stats = local;
        }
        return result;
    }

private:
    RocksDbGraphStore& store_;
};

}  // namespace low_latency_graphdb::storage
