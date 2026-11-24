#include "low_latency_graphdb/storage/rocksdb_graph_store.hpp"
#include "low_latency_graphdb/storage/storage_bfs.hpp"
#include "low_latency_graphdb/storage/tiered_graph.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace low_latency_graphdb;
using namespace low_latency_graphdb::storage;

namespace {

using AdjMap = std::unordered_map<NodeId, std::vector<NodeId>>;

std::vector<NodeId> reference_bfs(const AdjMap& adj, NodeId start, std::size_t max_depth) {
    std::vector<NodeId> visited;
    std::unordered_set<NodeId> seen{start};
    std::queue<std::pair<NodeId, std::size_t>> q;
    q.emplace(start, 0);
    while (!q.empty()) {
        const auto [u, depth] = q.front();
        q.pop();
        visited.push_back(u);
        if (depth >= max_depth) continue;
        if (const auto it = adj.find(u); it != adj.end()) {
            for (const NodeId v : it->second) {
                if (seen.insert(v).second) {
                    q.emplace(v, depth + 1);
                }
            }
        }
    }
    return visited;
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::abort();
    }
}

void test_key_ordering() {
    const auto a = encode_vertex_key(1);
    const auto b = encode_vertex_key(256);
    require(std::string(a.data(), a.size()) < std::string(b.data(), b.size()),
            "big-endian keys sort numerically");
    const auto edge = encode_edge_key(1, 2, 3);
    require(edge.size() == 18, "edge key is 18 bytes");
}

void test_store(const std::string& path) {
    {
        RocksDbGraphStore store(path);
        const std::vector<std::uint64_t> targets{2, 3, 4};
        const std::vector<EdgeLightMeta> metas{{1, 10, 100}, {1, 20, 200}, {2, 30, 300}};
        const std::vector<std::string> edge_payloads{"e2", "e3", "e4"};
        store.write_adjacency(1, targets, metas, edge_payloads);
        store.write_adjacency(2, std::vector<std::uint64_t>{4},
                              std::vector<EdgeLightMeta>{{1, 1, 1}});
        store.write_vertex_props(2, "v2");
        store.write_vertex_props(4, "v4");

        auto snapshot = store.snapshot();
        store.write_vertex_props(3, "v3-after-snapshot");

        RocksDbGraphStore::AdjacencyHandle handle;
        require(store.read_adjacency(1, handle, &snapshot), "adjacency found");
        require(handle.view().size() == 3, "degree");
        require(handle.view().target(2) == 4, "target");
        require(handle.view().meta(1).weight == 20, "meta");
        require(!store.read_adjacency(99, handle), "missing adjacency");

        const auto props = store.fetch_vertex_props({4, 2, 3, 2}, &snapshot);
        require(props.size() == 3, "deduplicated");
        require(props[0] == "v2" && props[1] == std::nullopt && props[2] == "v4",
                "snapshot isolates later writes");

        require(store.read_edge_props(1, 2, 4) == "e4", "edge props");
        require(!store.read_edge_props(1, 1, 4).has_value(), "edge key includes type");

        bool tripped = false;
        try {
            std::vector<std::uint64_t> many(RocksDbGraphStore::kMaxMaterializeBatchSize + 1);
            for (std::size_t i = 0; i < many.size(); ++i) many[i] = i;
            store.fetch_vertex_props(std::move(many));
        } catch (const StorageError&) {
            tripped = true;
        }
        require(tripped, "fan-out guard");
        store.flush();
    }

    // Reopen: data persisted and traversable directly via StorageBreadthFirstSearch.
    RocksDbGraphStore reopened(path);
    StorageBfsOptions reopen_opts;
    reopen_opts.max_depth = 2;
    const auto result = StorageBreadthFirstSearch(reopened).run(1, reopen_opts);
    std::vector<NodeId> visited = result.visited;
    std::sort(visited.begin(), visited.end());
    require((visited == std::vector<NodeId>{1, 2, 3, 4}), "BFS over reopened store");
}

void test_storage_bfs(const std::string& path) {
    RocksDbGraphStore store(path);
    // Tree with fanout 3 plus a back-edge and a self-loop.
    constexpr std::uint64_t kNodes = 400;
    AdjMap reference;
    for (std::uint64_t v = 0; v < kNodes; ++v) {
        std::vector<std::uint64_t> targets;
        for (std::uint64_t c = 3 * v + 1; c <= 3 * v + 3 && c < kNodes; ++c) {
            targets.push_back(c);
        }
        if (v > 0) targets.push_back(v - 1);
        if (v == 5) targets.push_back(5);
        std::vector<EdgeLightMeta> metas(targets.size());
        store.write_adjacency(v, targets, metas);
        reference[v] = targets;
    }
    store.flush();

    StorageBfsOptions options;
    options.max_depth = 3;
    options.pin_batch_size = 7;  // forces multiple chunks per level
    StorageBreadthFirstSearch bfs(store);
    StorageBfsStats stats;
    (void)bfs.run(0, options, &stats);  // warm the block cache
    const auto result = bfs.run(0, options, &stats);

    auto expected = reference_bfs(reference, 0, 3);
    auto actual = result.visited;
    std::sort(expected.begin(), expected.end());
    std::sort(actual.begin(), actual.end());
    require(actual == expected, "storage BFS matches reference BFS");
    require(stats.rows_pinned > 0, "adjacency rows are served from pinned pages");
    require(stats.rows_fetched == stats.rows_pinned + stats.rows_copied, "stats add up");

    // Concurrent readers sharing one store, plus a writer touching unrelated keys.
    std::atomic<int> mismatches{0};
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::uint64_t v = 10'000; !stop.load(); ++v) {
            store.write_adjacency(v, std::vector<std::uint64_t>{v + 1},
                                  std::vector<EdgeLightMeta>(1));
        }
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            for (int i = 0; i < 50; ++i) {
                auto got = bfs.run(0, options).visited;
                std::sort(got.begin(), got.end());
                if (got != expected) ++mismatches;
            }
        });
    }
    for (auto& r : readers) r.join();
    stop = true;
    writer.join();
    require(mismatches == 0, "concurrent storage BFS is consistent");

    bool tripped = false;
    options.max_visited = 10;
    try {
        (void)bfs.run(0, options);
    } catch (const StorageError&) {
        tripped = true;
    }
    require(tripped, "BFS visited guard");

    RocksDbGraphStore::AdjacencyBatch batch;
    bool rejected = false;
    try {
        const std::vector<std::uint64_t> unsorted{2, 1};
        store.read_adjacency_batch(unsorted, batch);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "unsorted batch rejected");
}

std::vector<NodeId> sorted_ids(std::vector<NodeId> ids) {
    std::sort(ids.begin(), ids.end());
    return ids;
}

void test_tiered_graph(const std::string& path) {
    RocksDbGraphStore store(path);
    constexpr std::uint64_t kNodes = 500;
    AdjMap reference;
    for (std::uint64_t v = 0; v < kNodes; ++v) {
        std::vector<std::uint64_t> targets;
        // Degree 15 for v % 50 == 0 exercises slab overflow (inline holds 11).
        const std::uint64_t fan = v % 50 == 0 ? 15 : 3;
        for (std::uint64_t c = fan * v + 1; c <= fan * v + fan && c < kNodes; ++c) {
            targets.push_back(c);
        }
        if (v > 0) targets.push_back(v / 3);
        std::vector<EdgeLightMeta> metas(targets.size());
        store.write_adjacency(v, targets, metas);
        reference[v] = targets;
    }
    store.flush();

    TieredGraph graph(store, 2048);
    TieredBfsState state;
    TieredBfsOptions options;
    options.pin_batch_size = 5;

    auto check_all = [&](const char* label) {
        for (std::size_t depth : {0u, 1u, 2u, 4u}) {
            options.max_depth = depth;
            for (NodeId start : {0ull, 1ull, 7ull, 50ull, 499ull}) {
                auto expected = sorted_ids(reference_bfs(reference, start, depth));
                std::vector<NodeId> got;
                const std::size_t count = graph.run(start, options, state, &got);
                if (sorted_ids(got) != expected || count != expected.size() ||
                    graph.run(start, options, state) != expected.size()) {
                    std::cerr << "mismatch: " << label << " start=" << start
                              << " depth=" << depth << '\n';
                    require(false, "tiered BFS matches reference BFS");
                }
            }
        }
    };

    // First pass loads vertices on demand into FixedSlabPool (no warm() needed).
    require(graph.hot_tier().stats().loaded_nodes == 0, "starts empty before first query");
    check_all("on-demand read-through");
    require(graph.hot_tier().stats().loaded_nodes > 0, "slabs populated on demand");
    require(graph.hot_tier().stats().overflow_edges > 0, "overflow edges stored");

    // Subsequent queries hit 100% in FixedSlabPool without touching RocksDB.
    TieredBfsStats stats;
    options.max_depth = 2;
    (void)graph.run(0, options, state, nullptr, &stats);
    require(stats.hot_rows > 0, "hot rows served from FixedSlabPool");
    require(stats.cold_rows == 0, "steady-state query never reads RocksDB");

    // Live COW write updates the cached slab in-place and retires the old slab via EBR.
    graph.write_adjacency(0, std::vector<std::uint64_t>{499},
                          std::vector<EdgeLightMeta>(1));
    options.max_depth = 1;
    std::vector<NodeId> got;
    graph.run(0, options, state, &got);
    require(sorted_ids(got) == (std::vector<NodeId>{0, 499}), "write visible immediately");
    (void)graph.run(0, options, state, nullptr, &stats);
    require(stats.cold_rows == 0 && stats.hot_rows == 1,
            "updated vertex stays hot via COW without RocksDB fallback");

    // Restore vertex 0 (degree 15 exercises overflow COW + EBR reclamation).
    std::vector<std::uint64_t> original_targets;
    for (std::uint64_t c = 1; c <= 15; ++c) original_targets.push_back(c);
    graph.write_adjacency(0, original_targets,
                          std::vector<EdgeLightMeta>(original_targets.size()));
    check_all("after COW restore");

    // Small-capacity pool still answers accurately via fallback without losing nodes.
    {
        TieredGraph small_graph(store, 20);
        TieredBfsState small_state;
        options.max_depth = 3;
        std::vector<NodeId> small_got;
        (void)small_graph.run(0, options, small_state, &small_got);
        require(sorted_ids(small_got) == sorted_ids(reference_bfs(reference, 0, 3)),
                "small slab pool falls back cleanly when capacity is reached");
    }

    // Concurrent readers + writer (updating both cached hot vertices and cold range).
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::thread writer([&] {
        // Repeatedly COW-update vertex 0 with its identical edges + write disjoint vertices.
        for (std::uint64_t step = 0; !stop.load(); ++step) {
            if ((step & 3u) == 0) {
                graph.write_adjacency(0, original_targets,
                                      std::vector<EdgeLightMeta>(original_targets.size()));
            } else {
                const std::uint64_t v = 100'000 + step;
                graph.write_adjacency(v, std::vector<std::uint64_t>{v + 1},
                                      std::vector<EdgeLightMeta>(1));
            }
        }
    });
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&] {
            TieredBfsState local_state;
            TieredBfsOptions local_options;
            local_options.max_depth = 3;
            const auto expected = reference_bfs(reference, 7, 3).size();
            for (int i = 0; i < 300; ++i) {
                if (graph.run(7, local_options, local_state) != expected) ++bad;
            }
        });
    }
    for (auto& r : readers) r.join();
    stop = true;
    writer.join();
    for (int i = 0; i < 3; ++i) (void)graph.hot_tier().try_reclaim();
    require(bad == 0, "concurrent read-through and EBR COW updates are consistent");
    require(graph.hot_tier().stats().retired_slots == 0,
            "all retired EBR slabs reclaimed once readers exit");

    // Arena hard quota guard: a tiny arena rejects a deep query and resets cleanly.
    TieredBfsState tiny_state(64);
    options.max_depth = 4;
    bool arena_oom = false;
    try {
        (void)graph.run(0, options, tiny_state);
    } catch (const std::runtime_error&) {
        arena_oom = true;
    }
    require(arena_oom, "tiny arena enforces per-query byte quota");
    options.max_depth = 1;
    require(graph.run(0, options, tiny_state) == 16,
            "arena resets cleanly after quota failure");
}

void test_write_update_visibility(const std::string& path) {
    constexpr NodeId a = 1;
    constexpr NodeId b = 2;
    constexpr NodeId c = 3;

    auto query_neighbors = [](const TieredGraph& graph, TieredBfsState& state,
                              TieredBfsStats& stats) {
        TieredBfsOptions opts;
        opts.max_depth = 1;
        std::vector<NodeId> visited;
        const std::size_t count = graph.run(a, opts, state, &visited, &stats);
        require(count == visited.size(), "visited count matches output size");
        return sorted_ids(std::move(visited));
    };

    {
        RocksDbGraphStore store(path);
        TieredGraph graph(store, 64);
        TieredBfsState state;
        TieredBfsStats stats;

        // 1. Write a -> b
        graph.write_adjacency(a, std::vector<NodeId>{b}, std::vector<EdgeLightMeta>(1));
        require(query_neighbors(graph, state, stats) == (std::vector<NodeId>{a, b}),
                "step 1 (first query): a -> b");
        require(stats.cold_rows == 1 && stats.hot_rows == 0,
                "step 1: first query loads a on demand from RocksDB");
        require(query_neighbors(graph, state, stats) == (std::vector<NodeId>{a, b}),
                "step 1 (cached query): a -> b");
        require(stats.hot_rows == 1 && stats.cold_rows == 0,
                "step 1: second query hits FixedSlabPool");

        // 2. Update to a -> b, a -> c
        graph.write_adjacency(a, std::vector<NodeId>{b, c}, std::vector<EdgeLightMeta>(2));
        require(query_neighbors(graph, state, stats) == (std::vector<NodeId>{a, b, c}),
                "step 2: updated to a -> b, a -> c");
        require(stats.hot_rows == 1 && stats.cold_rows == 0,
                "step 2: COW update keeps a hot without RocksDB fallback");

        // 3. Update to a -> c (removes a -> b)
        graph.write_adjacency(a, std::vector<NodeId>{c}, std::vector<EdgeLightMeta>(1));
        require(query_neighbors(graph, state, stats) == (std::vector<NodeId>{a, c}),
                "step 3: updated to a -> c (b removed)");
        require(stats.hot_rows == 1 && stats.cold_rows == 0,
                "step 3: COW update keeps a hot without RocksDB fallback");

        // Drain EBR epochs: both retired old slabs must be reclaimed.
        for (int i = 0; i < 3; ++i) (void)graph.hot_tier().try_reclaim();
        require(graph.hot_tier().stats().retired_slots == 0,
                "all retired slabs from a's updates reclaimed");
    }

    // 4. Reopen from RocksDB to verify final state (a -> c) is durable.
    {
        RocksDbGraphStore reopened(path);
        TieredGraph graph(reopened, 64);
        TieredBfsState state;
        TieredBfsStats stats;
        require(query_neighbors(graph, state, stats) == (std::vector<NodeId>{a, c}),
                "step 4: reopened store preserves final state a -> c");
    }
}

}  // namespace

int main() {
    const auto path = std::filesystem::temp_directory_path() / "llg_rocksdb_store_test";
    std::filesystem::remove_all(path);
    test_key_ordering();
    test_store(path.string());
    std::filesystem::remove_all(path);
    test_storage_bfs(path.string());
    std::filesystem::remove_all(path);
    test_tiered_graph(path.string());
    std::filesystem::remove_all(path);
    test_write_update_visibility(path.string());
    std::filesystem::remove_all(path);
    std::cout << "rocksdb store tests passed\n";
    return 0;
}
