#include "low_latency_graphdb/testing/breadth_first_search.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::size_t option(
    int argc,
    char** argv,
    std::string_view name,
    std::size_t fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] == name) {
            return std::stoull(argv[i + 1]);
        }
    }
    return fallback;
}

low_latency_graphdb::InMemoryGraph build_graph(
    std::size_t node_count,
    std::size_t degree) {
    low_latency_graphdb::InMemoryGraph graph;
    for (std::size_t node = 0; node < node_count; ++node) {
        graph.add_vertex(node);
    }

    for (std::size_t source = 0; source < node_count; ++source) {
        for (std::size_t edge = 1; edge <= degree; ++edge) {
            const auto target = (source + edge * 2654435761ULL) % node_count;
            graph.add_edge(source, target);
        }
    }
    return graph;
}

std::uint64_t percentile(
    const std::vector<std::uint64_t>& sorted_values,
    std::size_t percent) {
    const std::size_t index =
        ((sorted_values.size() - 1) * percent) / 100;
    return sorted_values[index];
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t node_count = option(argc, argv, "--nodes", 10'000);
    const std::size_t degree = option(argc, argv, "--degree", 8);
    const std::size_t query_count = option(argc, argv, "--queries", 2'000);
    const std::size_t depth = option(argc, argv, "--depth", 3);
    const std::size_t requested_threads =
        option(argc, argv, "--threads", std::thread::hardware_concurrency());

    if (node_count == 0 || degree == 0 || query_count == 0 ||
        requested_threads == 0) {
        std::cerr << "nodes, degree, queries, and threads must be positive\n";
        return EXIT_FAILURE;
    }

    const std::size_t worker_count =
        std::min(requested_threads, query_count);
    auto graph = build_graph(node_count, degree);
    low_latency_graphdb::BreadthFirstSearch search(graph);

    for (std::size_t query = 0; query < std::min<std::size_t>(100, query_count);
         ++query) {
        (void)search.run(query % node_count, depth);
    }

    std::vector<std::uint64_t> latencies_ns(query_count);
    std::vector<std::size_t> visited_counts(query_count);
    std::atomic<std::size_t> next_query{0};
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(worker_count + 1));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            start_barrier.arrive_and_wait();
            while (true) {
                const std::size_t query =
                    next_query.fetch_add(1, std::memory_order_relaxed);
                if (query >= query_count) {
                    break;
                }

                const auto started = Clock::now();
                const auto result = search.run(query % node_count, depth);
                const auto finished = Clock::now();
                latencies_ns[query] = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        finished - started)
                        .count());
                visited_counts[query] = result.visited.size();
            }
        });
    }

    const auto wall_started = Clock::now();
    start_barrier.arrive_and_wait();
    for (auto& worker : workers) {
        worker.join();
    }
    const auto wall_finished = Clock::now();

    std::sort(latencies_ns.begin(), latencies_ns.end());
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        wall_finished - wall_started);
    const std::uint64_t total_latency_ns =
        std::accumulate(latencies_ns.begin(), latencies_ns.end(), std::uint64_t{0});
    const std::size_t total_visited =
        std::accumulate(visited_counts.begin(), visited_counts.end(), std::size_t{0});
    const double wall_seconds =
        static_cast<double>(wall_ns.count()) / 1'000'000'000.0;

    std::cout << "graph.nodes=" << node_count
              << " graph.degree=" << degree
              << " queries=" << query_count
              << " depth=" << depth
              << " threads=" << worker_count << '\n';
    std::cout << std::fixed << std::setprecision(2)
              << "latency_us"
              << " p50=" << percentile(latencies_ns, 50) / 1'000.0
              << " p95=" << percentile(latencies_ns, 95) / 1'000.0
              << " p99=" << percentile(latencies_ns, 99) / 1'000.0
              << " max=" << latencies_ns.back() / 1'000.0
              << " mean=" << static_cast<double>(total_latency_ns) /
                                 static_cast<double>(query_count) / 1'000.0
              << '\n';
    std::cout << "throughput_qps=" << static_cast<double>(query_count) /
                                          wall_seconds
              << " avg_visited="
              << static_cast<double>(total_visited) /
                     static_cast<double>(query_count)
              << '\n';
}
