#include "low_latency_graphdb/graph_node_slab.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kNodeCount = 64;
constexpr std::uint32_t kFanout = 8;
constexpr std::uint32_t kDepth = 1;
using Clock = std::chrono::steady_clock;
using Node = low_latency_graphdb::GraphNodeSlab;
using LocalId = low_latency_graphdb::LocalNodeId;

struct CacheResidentGraph {
    std::array<Node, kNodeCount> nodes{};

    CacheResidentGraph() {
        for (std::size_t source = 0; source < kNodeCount; ++source) {
            nodes[source].node_id = source;
            nodes[source].count = kFanout;
            for (std::uint32_t edge = 0; edge < kFanout; ++edge) {
                nodes[source].inline_neighbors[edge] =
                    static_cast<LocalId>((source + edge + 1) % kNodeCount);
            }
        }
    }
};

struct TraversalState {
    std::array<LocalId, kNodeCount> queue{};
    std::array<std::uint32_t, kNodeCount> depths{};
    std::array<std::uint8_t, kNodeCount> visited{};
};

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
std::size_t run_query(
    const CacheResidentGraph& graph,
    TraversalState& state,
    LocalId start) noexcept {
    std::memset(state.visited.data(), 0, state.visited.size());
    state.queue[0] = start;
    state.depths[0] = 0;
    state.visited[start] = 1;

    std::size_t head = 0;
    std::size_t tail = 1;
    std::size_t visited_count = 0;
    while (head < tail) {
        const LocalId current = state.queue[head];
        const std::uint32_t depth = state.depths[head];
        ++head;
        ++visited_count;
        if (depth >= kDepth) {
            continue;
        }

        const Node& node = graph.nodes[current];
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const LocalId neighbor = node.inline_neighbors[i];
            if (state.visited[neighbor] == 0) {
                state.visited[neighbor] = 1;
                state.queue[tail] = neighbor;
                state.depths[tail] = depth + 1;
                ++tail;
            }
        }
    }
    return visited_count;
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t iterations =
        argc > 1 ? std::stoull(argv[1]) : 10'000'000;
    const std::size_t threads =
        argc > 2 ? std::stoull(argv[2]) : 1;
    if (iterations == 0 || threads == 0) {
        std::cerr << "iterations and threads must be positive\n";
        return EXIT_FAILURE;
    }

    const CacheResidentGraph graph;
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::uint64_t> checksums(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (std::size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&, thread] {
            TraversalState state;
            start_barrier.arrive_and_wait();
            std::uint64_t checksum = 0;
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                std::atomic_signal_fence(std::memory_order_acquire);
                checksum += run_query(graph, state, 0);
                std::atomic_signal_fence(std::memory_order_release);
            }
            checksums[thread] = checksum;
        });
    }

    const auto started = Clock::now();
    start_barrier.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto finished = Clock::now();

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started)
            .count();
    const std::uint64_t checksum =
        std::accumulate(checksums.begin(), checksums.end(), std::uint64_t{0});
    const std::size_t total_queries = iterations * threads;
    const double ns_per_query =
        static_cast<double>(elapsed_ns) / static_cast<double>(total_queries);

    std::cout << "cache_graph_nodes=" << kNodeCount
              << " graph_bytes=" << sizeof(graph)
              << " node_bytes=" << sizeof(Node)
              << " fanout=" << kFanout
              << " depth=" << kDepth
              << " iterations=" << iterations
              << " threads=" << threads << '\n';
    std::cout << std::fixed << std::setprecision(2)
              << "query_latency_ns=" << ns_per_query
              << " estimated_cycles_at_3GHz=" << ns_per_query * 3.0
              << " checksum=" << checksum << '\n';
}
