#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/query_arena.hpp"

#include <iostream>

int main() {
    low_latency_graphdb::QueryArena arena;
    low_latency_graphdb::FixedSlabPool hot_nodes(1024);
    std::cout << "low_latency_graphDB initialized "
              << "(arena_quota=" << arena.quota()
              << ", hot_node_capacity=" << 1024 << ")\n";
    return 0;
}
