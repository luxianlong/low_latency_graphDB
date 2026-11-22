#include "low_latency_graphdb/graph_node_slab.hpp"

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t scan_node(
    const low_latency_graphdb::GraphNodeSlab* node,
    std::size_t iterations) noexcept {
    std::uint64_t checksum = 0;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        std::atomic_signal_fence(std::memory_order_acquire);
        const std::uint32_t count = node->count;
        for (std::uint32_t i = 0; i < count; ++i) {
            checksum += node->inline_neighbors[i];
        }
        std::atomic_signal_fence(std::memory_order_release);
    }
    return checksum;
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

    std::vector<low_latency_graphdb::GraphNodeSlab> nodes(threads);
    for (auto& node : nodes) {
        node.count = 8;
        for (std::uint32_t i = 0; i < node.count; ++i) {
            node.inline_neighbors[i] = i + 1;
        }
    }

    std::barrier start_barrier(static_cast<std::ptrdiff_t>(threads + 1));
    std::vector<std::uint64_t> checksums(threads);
    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (std::size_t thread = 0; thread < threads; ++thread) {
        workers.emplace_back([&, thread] {
            start_barrier.arrive_and_wait();
            checksums[thread] = scan_node(&nodes[thread], iterations);
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
    const std::size_t total_scans = iterations * threads;
    const double ns_per_scan =
        static_cast<double>(elapsed_ns) / static_cast<double>(total_scans);

    std::cout << "cache_resident_node_size="
              << sizeof(low_latency_graphdb::GraphNodeSlab)
              << " bytes neighbors_per_scan=8 iterations=" << iterations
              << " threads=" << threads << '\n';
    std::cout << std::fixed << std::setprecision(2)
              << "scan_latency_ns=" << ns_per_scan
              << " estimated_cycles_at_3GHz=" << ns_per_scan * 3.0
              << " checksum=" << checksum << '\n';
}
