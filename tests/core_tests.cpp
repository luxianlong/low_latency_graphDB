#include "low_latency_graphdb/adjacency_view.hpp"
#include "low_latency_graphdb/graph_node_slab.hpp"
#include "low_latency_graphdb/query_arena.hpp"
#include "low_latency_graphdb/testing/breadth_first_search.hpp"
#include "low_latency_graphdb/testing/in_memory_graph.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using low_latency_graphdb::EdgeLightMeta;
using low_latency_graphdb::FixedSlabPool;
using low_latency_graphdb::GraphNodeSlab;
using low_latency_graphdb::QueryArena;
using low_latency_graphdb::ZeroCopyAdjView;

namespace {

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

void test_query_arena() {
    QueryArena arena(512);
    require(arena.capacity() == 512);
    arena.begin_query();
    auto* first = static_cast<std::uint64_t*>(arena.allocate(sizeof(std::uint64_t), alignof(std::uint64_t)));
    require(reinterpret_cast<std::uintptr_t>(first) % alignof(std::uint64_t) == 0);
    *first = 42;
    require(*first == 42);

    auto* line = arena.allocate(64, 64);
    require(reinterpret_cast<std::uintptr_t>(line) % 64 == 0);
    require(arena.used_bytes() >= 72);

    bool begin_failed = false;
    try {
        arena.begin_query();
    } catch (const std::logic_error&) {
        begin_failed = true;
    }
    require(begin_failed);

    arena.reset();
    require(arena.used_bytes() == 0);
    arena.begin_query();

    // Capacity is the hard quota: no growth, the request is refused.
    bool quota_failed = false;
    try {
        (void)arena.allocate(513, 1);
    } catch (const std::runtime_error&) {
        quota_failed = true;
    }
    require(quota_failed);
    require(arena.used_bytes() == 0);

    // STL adapter draws from the arena; ArenaScope resets it on exit.
    QueryArena pool(1U << 20);
    {
        low_latency_graphdb::ArenaScope scope(pool);
        std::vector<int, low_latency_graphdb::ArenaAllocator<int>> values{
            low_latency_graphdb::ArenaAllocator<int>(&pool)};
        for (int i = 0; i < 100; ++i) {
            values.push_back(i);
        }
        require(values[99] == 99);
        require(pool.used_bytes() > 0);
    }
    require(pool.used_bytes() == 0);
    require(&low_latency_graphdb::thread_arena() == &low_latency_graphdb::thread_arena());
}

void test_slab_pool() {
    static_assert(sizeof(GraphNodeSlab) == 64);
    static_assert(sizeof(low_latency_graphdb::SlabSlot) == 64);
    FixedSlabPool pool(2);
    require(pool.capacity() == 2);
    auto* first = pool.allocate();
    auto* second = pool.allocate();
    require(first != nullptr);
    require(second != nullptr);
    require(reinterpret_cast<std::uintptr_t>(first) % 64 == 0);
    require(reinterpret_cast<std::uintptr_t>(second) % 64 == 0);
    require(pool.owns(first) && pool.owns(second));
    require(pool.allocate() == nullptr);
    pool.release(first);
    require(pool.available() == 1);
    pool.release(second);
    require(pool.available() == 2);

    // Direct 3-bucket retire/reclaim.
    auto* a = pool.allocate();
    a->node_id = 111;
    auto* b = pool.allocate();
    b->node_id = 222;
    require(pool.available() == 0);
    pool.retire_slot(a, 0);
    pool.retire_slot(b, 1);
    require(pool.available() == 0);
    require(pool.retired_count() == 2);
    require(pool.reclaim_epoch(2) == 0);
    require(pool.reclaim_epoch(0) == 1);
    require(pool.available() == 1);
    require(pool.retired_count() == 1);
    require(pool.reclaim_epoch(1) == 1);
    require(pool.available() == 2);
    require(pool.retired_count() == 0);

    // EpochTracker blocks reclamation while a reader holds an older epoch.
    low_latency_graphdb::EpochTracker tracker;
    auto reader = tracker.register_reader();
    auto* live = pool.allocate();
    live->node_id = 777;
    {
        low_latency_graphdb::EpochTracker::ReadGuard guard(tracker, reader);
        tracker.retire(pool, live);  // retired in epoch 0 while reader is in epoch 0
        require(tracker.try_advance(pool) == 0);  // advances 0 -> 1, epoch 0 not safe yet
        require(tracker.current_epoch() == 1);
        // Reader is still in epoch 0 < current_epoch(1), so epoch 1 cannot advance.
        require(tracker.try_advance(pool) == 0);
        require(pool.available() == 1);
        require(live->node_id == 777);  // still untouched while reader is active
    }
    // Reader exited: advancing 1 -> 2 reclaims epoch 0.
    require(tracker.try_advance(pool) == 1);
    require(pool.available() == 2);
    require(pool.retired_count() == 0);

    // Concurrent readers + writer replacing and retiring slabs under EBR.
    FixedSlabPool stress_pool(128);
    low_latency_graphdb::EpochTracker stress_tracker;
    auto* init = stress_pool.allocate();
    init->node_id = 1;
    init->count = 1;
    init->inline_neighbors[0] = 42;
    std::atomic<GraphNodeSlab*> published{init};
    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};

    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            auto h = stress_tracker.register_reader();
            while (!stop.load(std::memory_order_relaxed)) {
                low_latency_graphdb::EpochTracker::ReadGuard g(stress_tracker, h);
                const GraphNodeSlab* p = published.load(std::memory_order_acquire);
                if (p == nullptr || p->count != 1 || p->inline_neighbors[0] != 42 ||
                    p->node_id == 0) {
                    ++bad;
                }
            }
        });
    }
    for (std::uint64_t step = 2; step <= 2000; ++step) {
        GraphNodeSlab* next = stress_pool.allocate();
        while (next == nullptr) {
            (void)stress_tracker.try_advance(stress_pool);
            next = stress_pool.allocate();
        }
        next->node_id = step;
        next->count = 1;
        next->inline_neighbors[0] = 42;
        GraphNodeSlab* old = published.exchange(next, std::memory_order_acq_rel);
        stress_tracker.retire(stress_pool, old);
        if ((step & 7u) == 0) {
            (void)stress_tracker.try_advance(stress_pool);
        }
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();
    require(bad.load() == 0);
    // Drain remaining retired slots.
    for (int i = 0; i < 3; ++i) (void)stress_tracker.try_advance(stress_pool);
    require(stress_pool.retired_count() == 0);
    require(stress_pool.available() == 127);  // only the currently published slab remains
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
    require(view.size() == 2);
    require(view.target(0) == 101);
    require(view.target(1) == 202);
    require(view.meta(1).edge_type == 8);

    bool bounds_failed = false;
    try {
        (void)view.target(2);
    } catch (const std::out_of_range&) {
        bounds_failed = true;
    }
    require(bounds_failed);

    std::vector<char> truncated(buffer.begin(), buffer.end() - 1);
    bool truncated_failed = false;
    try {
        ZeroCopyAdjView invalid(truncated.data(), truncated.size());
    } catch (const std::invalid_argument&) {
        truncated_failed = true;
    }
    require(truncated_failed);
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
    require((depth_two.visited == std::vector<std::uint64_t>{1, 2, 3, 4}));

    const auto depth_zero = search.run(1, 0);
    require((depth_zero.visited == std::vector<std::uint64_t>{1}));

    const auto missing = search.run(999, 10);
    require(missing.visited.empty());

    low_latency_graphdb::InMemoryGraph high_degree;
    high_degree.add_vertex(100);
    for (std::uint64_t id = 0; id < 12; ++id) {
        high_degree.add_edge(100, 1000 + id);
    }
    const auto overflow_result =
        low_latency_graphdb::BreadthFirstSearch(high_degree).run(100, 1);
    require(overflow_result.visited.size() == 13);
    require(high_degree.hot_node_storage_bytes() == 13 * sizeof(GraphNodeSlab));
}

}  // namespace

int main() {
    test_query_arena();
    test_slab_pool();
    test_adjacency_view();
    test_breadth_first_search();
    std::cout << "core tests passed\n";
}
