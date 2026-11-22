#include "low_latency_graphdb/graph_node_slab.hpp"

#include <algorithm>
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

constexpr std::uint32_t kFanout = 8;
constexpr std::size_t kVisitedCapacity = 2048;
using Clock = std::chrono::steady_clock;
using Node = low_latency_graphdb::GraphNodeSlab;
using LocalId = low_latency_graphdb::LocalNodeId;

struct CacheResidentGraph {
    explicit CacheResidentGraph(std::size_t node_count)
        : nodes(node_count) {
        for (std::size_t source = 0; source < nodes.size(); ++source) {
            nodes[source].node_id = source;
            const std::size_t first_child = source * kFanout + 1;
            if (first_child >= nodes.size()) {
                continue;
            }
            nodes[source].count = static_cast<std::uint32_t>(
                std::min<std::size_t>(kFanout, nodes.size() - first_child));
            for (std::uint32_t edge = 0; edge < nodes[source].count; ++edge) {
                nodes[source].inline_neighbors[edge] =
                    static_cast<LocalId>(first_child + edge);
            }
        }
    }

    std::vector<Node> nodes;
};

struct TraversalState {
    struct SeenEntry {
        LocalId node{0};
        std::uint32_t generation{0};
    };

    std::array<LocalId, kVisitedCapacity> queue{};
    std::array<std::uint32_t, kVisitedCapacity> depths{};
    std::array<SeenEntry, kVisitedCapacity> seen{};
    std::uint32_t generation{0};

    void begin_query() noexcept {
        ++generation;
        if (generation == 0) {
            for (auto& entry : seen) {
                entry.generation = 0;
            }
            generation = 1;
        }
    }

    bool mark_seen(LocalId node) noexcept {
        constexpr std::size_t mask = kVisitedCapacity - 1;
        std::size_t slot = (static_cast<std::size_t>(node) * 2654435761U) & mask;
        for (std::size_t probe = 0; probe < kVisitedCapacity; ++probe) {
            auto& entry = seen[slot];
            if (entry.generation != generation) {
                entry.node = node;
                entry.generation = generation;
                return true;
            }
            if (entry.node == node) {
                return false;
            }
            slot = (slot + 1) & mask;
        }
        return false;
    }
};

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted_values,
    std::size_t percent) {
    const std::size_t index =
        ((sorted_values.size() - 1) * percent) / 100;
    return sorted_values[index];
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
std::size_t run_query(
    const CacheResidentGraph& graph,
    TraversalState& state,
    LocalId start,
    std::uint32_t max_depth) noexcept {
    state.begin_query();
    state.queue[0] = start;
    state.depths[0] = 0;
    state.mark_seen(start);

    std::size_t head = 0;
    std::size_t tail = 1;
    std::size_t visited_count = 0;
    while (head < tail) {
        const LocalId current = state.queue[head];
        const std::uint32_t depth = state.depths[head];
        ++head;
        ++visited_count;
        if (depth >= max_depth) {
            continue;
        }

        const Node& node = graph.nodes[current];
        for (std::uint32_t i = 0; i < node.count; ++i) {
            const LocalId neighbor = node.inline_neighbors[i];
            if (state.mark_seen(neighbor)) {
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
        argc > 1 ? std::stoull(argv[1]) : 10'000;
    const std::size_t threads =
        argc > 2 ? std::stoull(argv[2]) : 1;
    const std::uint32_t depth =
        argc > 3 ? static_cast<std::uint32_t>(std::stoull(argv[3])) : 1;
    const std::size_t node_count =
        argc > 4 ? std::stoull(argv[4]) : 1'000'000;
    const std::size_t batch_size =
        argc > 5 ? std::stoull(argv[5]) : 64;
    if (iterations == 0 || threads == 0 || node_count == 0 ||
        batch_size == 0) {
        std::cerr << "iterations, threads, nodes, and batch size must be positive\n";
        return EXIT_FAILURE;
    }

    const CacheResidentGraph graph(node_count);
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::uint64_t> checksums(threads);
    std::vector<std::vector<std::uint64_t>> thread_latencies(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);

    for (std::size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&, thread] {
            TraversalState state;
            auto& latencies = thread_latencies[thread];
            latencies.reserve(iterations);
            start_barrier.arrive_and_wait();
            std::uint64_t checksum = 0;
            for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
                std::atomic_signal_fence(std::memory_order_acquire);
                const auto query_started = Clock::now();
                for (std::size_t query = 0; query < batch_size; ++query) {
                    checksum += run_query(graph, state, 0, depth);
                }
                const auto query_finished = Clock::now();
                std::atomic_signal_fence(std::memory_order_release);
                latencies.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        query_finished - query_started)
                        .count() /
                    batch_size));
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
    const std::size_t total_queries = iterations * threads * batch_size;
    std::vector<std::uint64_t> latencies;
    latencies.reserve(total_queries);
    for (auto& thread_latencies : thread_latencies) {
        latencies.insert(
            latencies.end(),
            thread_latencies.begin(),
            thread_latencies.end());
    }
    std::sort(latencies.begin(), latencies.end());
    const std::uint64_t total_latency_ns =
        std::accumulate(latencies.begin(), latencies.end(), std::uint64_t{0});
    const double mean_latency_ns =
        static_cast<double>(total_latency_ns) /
        static_cast<double>(latencies.size());
    const double wall_ns_per_query =
        static_cast<double>(elapsed_ns) / static_cast<double>(total_queries);

    std::cout     << "cache_graph_nodes=" << graph.nodes.size()
    << " graph_bytes=" << graph.nodes.size() * sizeof(Node)
              << " node_bytes=" << sizeof(Node)
              << " fanout=" << kFanout
              << " depth=" << depth
              << " iterations=" << iterations
              << " threads=" << threads
              << " queries_per_sample=" << batch_size << '\n';
    std::cout << std::fixed << std::setprecision(2)
              << "request_latency_ns"
              << " p50=" << percentile(latencies, 50)
              << " p95=" << percentile(latencies, 95)
              << " p99=" << percentile(latencies, 99)
              << " max=" << latencies.back()
              << " mean=" << mean_latency_ns
              << " wall_amortized=" << wall_ns_per_query
              << " throughput_qps=" << 1'000'000'000.0 / wall_ns_per_query
              << " checksum=" << checksum << '\n';
}
