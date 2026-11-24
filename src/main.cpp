#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/query_arena.hpp"

#include <iostream>
#include <vector>

#ifdef LLG_ENABLE_ROCKSDB
#include "low_latency_graphdb/storage/rocksdb_graph_store.hpp"
#include "low_latency_graphdb/storage/tiered_graph.hpp"

#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    using low_latency_graphdb::EdgeLightMeta;
    using low_latency_graphdb::NodeId;
    using low_latency_graphdb::storage::RocksDbGraphStore;
    using low_latency_graphdb::storage::TieredBfsOptions;
    using low_latency_graphdb::storage::TieredBfsState;
    using low_latency_graphdb::storage::TieredBfsStats;
    using low_latency_graphdb::storage::TieredGraph;

    const bool use_temp = argc <= 1;
    const std::filesystem::path db_path =
        use_temp ? (std::filesystem::temp_directory_path() / "llg_demo_db")
                 : std::filesystem::path(argv[1]);
    if (use_temp) {
        std::filesystem::remove_all(db_path);
    }

    {
        RocksDbGraphStore store(db_path.string());
        TieredGraph graph(store, 1024);

        // Write topology (cf_topo) and vertex properties (cf_vertex_props).
        graph.write_adjacency(1, std::vector<NodeId>{2, 3}, std::vector<EdgeLightMeta>(2));
        graph.write_adjacency(2, std::vector<NodeId>{4}, std::vector<EdgeLightMeta>(1));
        store.write_vertex_props(1, "root");
        store.write_vertex_props(4, "leaf");

        TieredBfsState state;
        TieredBfsOptions options;
        options.max_depth = 2;
        TieredBfsStats stats;
        std::vector<NodeId> visited;

        // First query: read-through loads touched slabs from RocksDB into FixedSlabPool.
        graph.run(1, options, state, &visited, &stats);
        std::cout << "read-through BFS(1, depth=2): visited=" << visited.size()
                  << " hot_rows=" << stats.hot_rows << " cold_rows=" << stats.cold_rows << "\n";

        // Second query: served 100% from FixedSlabPool using the per-thread QueryArena.
        visited.clear();
        graph.run(1, options, state, &visited, &stats);
        std::cout << "cached BFS(1, depth=2):       visited=" << visited.size()
                  << " hot_rows=" << stats.hot_rows << " cold_rows=" << stats.cold_rows << "\n";

        // Live COW write updates FixedSlabPool in place (retiring the old slab via EBR).
        graph.write_adjacency(1, std::vector<NodeId>{2, 3, 5}, std::vector<EdgeLightMeta>(3));
        visited.clear();
        graph.run(1, options, state, &visited, &stats);
        std::cout << "after COW write(1->2,3,5):    visited=" << visited.size()
                  << " hot_rows=" << stats.hot_rows << " cold_rows=" << stats.cold_rows << "\n";

        // Late materialization: fetch vertex properties only for final traversal results.
        const auto props = store.fetch_vertex_props(visited);
        std::size_t with_props = 0;
        for (const auto& p : props) {
            if (p.has_value()) {
                ++with_props;
            }
        }
        const auto tier_stats = graph.hot_tier().stats();
        std::cout << "late-materialized props=" << with_props
                  << " loaded_slabs=" << tier_stats.loaded_nodes
                  << " pool_capacity=" << tier_stats.pool_capacity << "\n";
    }

    if (use_temp) {
        std::filesystem::remove_all(db_path);
    }
    return 0;
}
#else
int main() {
    low_latency_graphdb::QueryArena arena;
    low_latency_graphdb::FixedSlabPool hot_nodes(1024);

    std::cout << "low_latency_graphDB built without RocksDB "
              << "(arena_capacity=" << arena.capacity()
              << ", hot_node_capacity=" << hot_nodes.capacity() << ")\n";
    return 0;
}
#endif
