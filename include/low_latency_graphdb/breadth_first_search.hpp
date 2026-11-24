#pragma once

#include "low_latency_graphdb/in_memory_graph.hpp"
#include "low_latency_graphdb/query_arena.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace low_latency_graphdb {

struct TraversalResult {
    std::vector<NodeId> visited;
};

class BreadthFirstSearch {
public:
    explicit BreadthFirstSearch(const InMemoryGraph& graph)
        : graph_(graph) {}

    [[nodiscard]] TraversalResult run(NodeId start, std::size_t max_depth) const {
        TraversalResult result;
        if (!graph_.contains(start) || graph_.size() == 0) {
            return result;
        }

        QueryArena& arena = thread_arena();
        ArenaScope arena_scope(arena);
        const std::size_t node_count = graph_.size();
        auto* queue = static_cast<LocalNodeId*>(
            arena.allocate(node_count * sizeof(LocalNodeId), alignof(LocalNodeId)));
        auto* depths = static_cast<std::uint32_t*>(
            arena.allocate(node_count * sizeof(std::uint32_t), alignof(std::uint32_t)));
        auto* visited = static_cast<std::uint8_t*>(
            arena.allocate(node_count, alignof(std::uint8_t)));
        std::memset(visited, 0, node_count);

        const LocalNodeId start_local = graph_.local_id(start);
        queue[0] = start_local;
        depths[0] = 0;
        visited[start_local] = 1;
        std::size_t head = 0;
        std::size_t tail = 1;
        result.visited.reserve(node_count);

        while (head < tail) {
            const LocalNodeId current = queue[head];
            const std::uint32_t current_depth = depths[head];
            ++head;
            result.visited.push_back(graph_.node_id(current));

            if (current_depth >= max_depth ||
                current_depth == std::numeric_limits<std::uint32_t>::max()) {
                continue;
            }

            graph_.for_each_neighbor(current, [&](LocalNodeId neighbor) {
                if (visited[neighbor] != 0) {
                    return;
                }
                visited[neighbor] = 1;
                queue[tail] = neighbor;
                depths[tail] = current_depth + 1;
                ++tail;
            });
        }

        return result;
    }

private:
    const InMemoryGraph& graph_;
};

}  // namespace low_latency_graphdb
