#include "low_latency_graphdb/adjacency_view.hpp"
#include "low_latency_graphdb/breadth_first_search.hpp"
#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/in_memory_graph.hpp"
#include "low_latency_graphdb/query_arena.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

using low_latency_graphdb::EdgeLightMeta;
using low_latency_graphdb::FixedSlabPool;
using low_latency_graphdb::GraphNodeSlab;
using low_latency_graphdb::QueryArena;
using low_latency_graphdb::ZeroCopyAdjView;

namespace {

void test_query_arena() {
    QueryArena arena(128, 512);
    auto* first = static_cast<std::uint64_t*>(arena.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
    assert(reinterpret_cast<std::uintptr_t>(first) % alignof(std::uint64_t) == 0);
    *first = 42;
    assert(*first == 42);

    auto* overflow = arena.allocate(200, 64);
    assert(overflow != nullptr);
    assert(arena.used_bytes() >= 200);

    arena.reset();
    assert(arena.used_bytes() == 0);

    bool quota_failed = false;
    try {
        (void)arena.allocate(513, 1);
    } catch (const std::runtime_error&) {
        quota_failed = true;
    }
    assert(quota_failed);
}

void test_slab_pool() {
    static_assert(sizeof(GraphNodeSlab) == 64);
    FixedSlabPool pool(2);
    auto* first = pool.allocate();
    auto* second = pool.allocate();
    assert(first != nullptr);
    assert(second != nullptr);
    assert(pool.allocate() == nullptr);
    pool.release(first);
    assert(pool.available() == 1);
    pool.release(second);
    assert(pool.available() == 2);
}

std::vector<char> make_adjacency_buffer() {
    const std::uint32_t count = 2;
    const std::uint64_t targets[] = {101, 202};
    const EdgeLightMeta metas[] = {{7, 3, 1000}, {8, 4, 2000}};
    std::vector<char> buffer(
        sizeof(count) + sizeof(targets) + sizeof(metas), '\0');
    std::memcpy(buffer.data(), &count, sizeof(count));
    std::memcpy(buffer.data() + sizeof(count), targets, sizeof(targets));
    std::memcpy(buffer.data() + sizeof(count) + sizeof(targets), metas, sizeof(metas));
    return buffer;
}

void test_adjacency_view() {
    auto buffer = make_adjacency_buffer();
    ZeroCopyAdjView view(buffer.data(), buffer.size());
    assert(view.size() == 2);
    assert(view.target(0) == 101);
    assert(view.target(1) == 202);
    assert(view.edge_metas()[1].edge_type == 8);

    bool bounds_failed = false;
    try {
        (void)view.target(2);
    } catch (const std::out_of_range&) {
        bounds_failed = true;
    }
    assert(bounds_failed);

    std::vector<char> truncated(buffer.begin(), buffer.end() - 1);
    bool truncated_failed = false;
    try {
        ZeroCopyAdjView invalid(truncated.data(), truncated.size());
    } catch (const std::invalid_argument&) {
        truncated_failed = true;
    }
    assert(truncated_failed);
}

void test_breadth_first_search() {
    low_latency_graphdb::InMemoryGraph graph;
    graph.add_edge(1, 2);
    graph.add_edge(1, 3);
    graph.add_edge(2, 4);
    graph.add_edge(3, 4);
    graph.add_edge(4, 5);
    graph.add_edge(5, 1);

    low_latency_graphdb::BreadthFirstSearch search(graph);
    const auto depth_two = search.run(1, 2);
    assert((depth_two.visited == std::vector<std::uint64_t>{1, 2, 3, 4}));

    const auto depth_zero = search.run(1, 0);
    assert((depth_zero.visited == std::vector<std::uint64_t>{1}));

    const auto missing = search.run(999, 10);
    assert(missing.visited.empty());

    low_latency_graphdb::InMemoryGraph high_degree;
    high_degree.add_vertex(100);
    for (std::uint64_t id = 0; id < 12; ++id) {
        high_degree.add_edge(100, 1000 + id);
    }
    const auto overflow_result =
        low_latency_graphdb::BreadthFirstSearch(high_degree).run(100, 1);
    assert(overflow_result.visited.size() == 13);
}

}  // namespace

int main() {
    test_query_arena();
    test_slab_pool();
    test_adjacency_view();
    test_breadth_first_search();
    std::cout << "core tests passed\n";
}
