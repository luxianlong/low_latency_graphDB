#pragma once

#include "low_latency_graphdb/graph_node_slab.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace low_latency_graphdb {

class InMemoryGraph {
public:
    [[nodiscard]] bool contains(NodeId node_id) const noexcept {
        return index_by_id_.find(node_id) != index_by_id_.end();
    }

    void add_vertex(NodeId node_id) {
        if (contains(node_id)) {
            return;
        }
        if (nodes_.size() >= std::numeric_limits<LocalNodeId>::max()) {
            throw std::length_error("InMemoryGraph: local node ID space exhausted");
        }

        const auto local_id = static_cast<LocalNodeId>(nodes_.size());
        index_by_id_.emplace(node_id, local_id);
        nodes_.emplace_back();
        overflow_neighbors_.emplace_back();
        nodes_.back().node_id = node_id;
    }

    void add_edge(NodeId source, NodeId target) {
        add_vertex(source);
        add_vertex(target);

        const LocalNodeId source_local = index_by_id_.at(source);
        const LocalNodeId target_local = index_by_id_.at(target);
        GraphNodeSlab& node = nodes_[source_local];
        if (node.count == std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("InMemoryGraph: vertex degree exhausted");
        }

        if (node.count < node.inline_neighbors.size()) {
            node.inline_neighbors[node.count] = target_local;
        } else {
            if (!node.has_overflow()) {
                node.overflow_index = source_local + 1;
            }
            overflow_neighbors_[source_local].push_back(target_local);
        }
        ++node.count;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return nodes_.size();
    }

    [[nodiscard]] LocalNodeId local_id(NodeId node_id) const {
        return index_by_id_.at(node_id);
    }

    [[nodiscard]] NodeId node_id(LocalNodeId local_id) const {
        if (local_id >= nodes_.size()) {
            throw std::out_of_range("InMemoryGraph: local node ID out of bounds");
        }
        return nodes_[local_id].node_id;
    }

    template <typename Visitor>
    void for_each_neighbor(LocalNodeId local_id, Visitor&& visitor) const {
        if (local_id >= nodes_.size()) {
            throw std::out_of_range("InMemoryGraph: local node ID out of bounds");
        }

        const GraphNodeSlab& node = nodes_[local_id];
        const std::size_t inline_count =
            std::min<std::size_t>(node.count, node.inline_neighbors.size());
        for (std::size_t i = 0; i < inline_count; ++i) {
            visitor(node.inline_neighbors[i]);
        }
        if (node.has_overflow()) {
            for (LocalNodeId neighbor : overflow_neighbors_[local_id]) {
                visitor(neighbor);
            }
        }
    }

private:
    std::unordered_map<NodeId, LocalNodeId> index_by_id_;
    std::vector<GraphNodeSlab> nodes_;
    std::vector<std::vector<LocalNodeId>> overflow_neighbors_;
};

}  // namespace low_latency_graphdb
