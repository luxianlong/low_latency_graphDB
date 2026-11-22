#include "low_latency_graphdb/graph_node_slab.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

constexpr std::uint32_t kFanout = 8;
constexpr std::uint32_t kDepth = 2;
constexpr std::size_t kExpectedVisited = 73;
constexpr std::size_t kGraphNodes = 640'009;
using Node = low_latency_graphdb::GraphNodeSlab;
using LocalId = low_latency_graphdb::LocalNodeId;

std::uint64_t mix_hot_root(std::uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

struct TestGraph {
    TestGraph() : nodes(kGraphNodes) {
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

std::size_t run_query(
    const TestGraph& graph,
    std::vector<std::uint32_t>& seen,
    std::uint32_t generation,
    LocalId start) {
    std::array<LocalId, kExpectedVisited> queue{};
    std::array<std::uint32_t, kExpectedVisited> depths{};
    queue[0] = start;
    depths[0] = 0;
    seen[start] = generation;

    std::size_t head = 0;
    std::size_t tail = 1;
    std::size_t visited = 0;
    while (head < tail) {
        const LocalId current = queue[head];
        const std::uint32_t depth = depths[head];
        ++head;
        ++visited;
        if (depth >= kDepth) {
            continue;
        }

        const Node& node = graph.nodes[current];
        for (std::uint32_t edge = 0; edge < node.count; ++edge) {
            const LocalId neighbor = node.inline_neighbors[edge];
            if (seen[neighbor] != generation) {
                seen[neighbor] = generation;
                queue[tail] = neighbor;
                depths[tail] = depth + 1;
                ++tail;
            }
        }
    }
    return visited;
}

bool verify_expected_nodes(
    const TestGraph& graph,
    const std::vector<std::uint32_t>& seen,
    std::uint32_t generation,
    LocalId root,
    LocalId& mismatch) {
    const auto verify = [&](std::size_t node) {
        if (graph.nodes[node].node_id != node || seen[node] != generation) {
            mismatch = static_cast<LocalId>(node);
            return false;
        }
        return true;
    };

    if (!verify(root)) {
        return false;
    }
    for (std::uint32_t child = 0; child < kFanout; ++child) {
        const std::size_t child_id =
            static_cast<std::size_t>(root) * kFanout + child + 1;
        if (!verify(child_id)) {
            return false;
        }
        for (std::uint32_t grandchild = 0;
             grandchild < kFanout;
             ++grandchild) {
            const std::size_t grandchild_id =
                child_id * kFanout + grandchild + 1;
            if (!verify(grandchild_id)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

int main() {
    const TestGraph graph;
    std::vector<std::uint32_t> seen(graph.nodes.size(), 0);
    constexpr std::array<std::size_t, 3> hot_node_counts{10, 100, 10'000};

    for (const std::size_t hot_nodes : hot_node_counts) {
        std::fill(seen.begin(), seen.end(), 0);
        const std::size_t query_count = hot_nodes;
        bool passed = true;
        std::size_t failed_root = 0;
        std::size_t failed_visited = 0;
        std::size_t failed_node = 0;
        for (std::size_t query = 0; query < query_count; ++query) {
            const LocalId root = static_cast<LocalId>(
                mix_hot_root(query) % hot_nodes);
            const auto generation = static_cast<std::uint32_t>(query + 1);
            const std::size_t visited = run_query(
                graph, seen, generation, root);
            LocalId mismatch = 0;
            if (visited != kExpectedVisited ||
                !verify_expected_nodes(
                    graph, seen, generation, root, mismatch)) {
                passed = false;
                failed_root = root;
                failed_visited = visited;
                failed_node = mismatch;
                break;
            }
        }

        std::cout << "depth=" << kDepth
                  << " hot_nodes=" << hot_nodes
                  << " queries=" << query_count
                  << " expected_visited=" << kExpectedVisited
                  << " exact_nodes=" << (passed ? "PASS" : "FAIL")
                  << " result=" << (passed ? "PASS" : "FAIL");
        if (!passed) {
            std::cout << " failed_root=" << failed_root
                      << " actual_visited=" << failed_visited
                      << " missing_or_wrong_node=" << failed_node;
        }
        std::cout << '\n';
        if (!passed) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
