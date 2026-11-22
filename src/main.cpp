#include "low_latency_graphdb/breadth_first_search.hpp"
#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/query_arena.hpp"

#include <iostream>

int main() {
    low_latency_graphdb::QueryArena arena;
    low_latency_graphdb::FixedSlabPool hot_nodes(1024);
    low_latency_graphdb::InMemoryGraph graph;
    graph.add_edge(1, 2);
    graph.add_edge(1, 3);
    graph.add_edge(2, 4);
    const auto traversal =
        low_latency_graphdb::BreadthFirstSearch(graph).run(1, 2);

    std::cout << "low_latency_graphDB initialized "
              << "(arena_quota=" << arena.quota()
              << ", hot_node_capacity=" << 1024
              << ", traversal_nodes=" << traversal.visited.size() << ")\n";
    return 0;
}
