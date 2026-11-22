#pragma once

#include "low_latency_graphdb/adjacency_view.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace low_latency_graphdb::storage {

// Keys are big-endian so RocksDB's bytewise comparator orders them by numeric
// ID, which keeps sorted MultiGet batches in physical SST order.
constexpr std::uint64_t to_big_endian(std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap64(value);
    } else {
        return value;
    }
}

constexpr std::uint16_t to_big_endian(std::uint16_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return __builtin_bswap16(value);
    } else {
        return value;
    }
}

using VertexKey = std::array<char, sizeof(std::uint64_t)>;
// [SrcID (8B) | EdgeType (2B) | DstID (8B)], all big-endian.
using EdgeKey = std::array<char, 18>;

inline VertexKey encode_vertex_key(std::uint64_t node_id) noexcept {
    VertexKey key;
    const std::uint64_t be = to_big_endian(node_id);
    std::memcpy(key.data(), &be, sizeof(be));
    return key;
}

inline EdgeKey encode_edge_key(std::uint64_t src, std::uint16_t edge_type,
                               std::uint64_t dst) noexcept {
    EdgeKey key;
    const std::uint64_t be_src = to_big_endian(src);
    const std::uint16_t be_type = to_big_endian(edge_type);
    const std::uint64_t be_dst = to_big_endian(dst);
    std::memcpy(key.data(), &be_src, 8);
    std::memcpy(key.data() + 8, &be_type, 2);
    std::memcpy(key.data() + 10, &be_dst, 8);
    return key;
}

// cf_topo value: [Count:4B | TargetIDs[count] | EdgeLightMetas[count]]
// (native-endian payload, matching what ZeroCopyAdjView decodes).
inline std::string encode_adjacency(std::span<const std::uint64_t> targets,
                                    std::span<const EdgeLightMeta> metas) {
    if (targets.size() != metas.size()) {
        throw std::invalid_argument("encode_adjacency: targets/metas size mismatch");
    }
    if (targets.size() > UINT32_MAX) {
        throw std::length_error("encode_adjacency: degree exceeds 32 bits");
    }
    const auto count = static_cast<std::uint32_t>(targets.size());
    std::string value;
    value.reserve(sizeof(count) + targets.size_bytes() + metas.size_bytes());
    value.append(reinterpret_cast<const char*>(&count), sizeof(count));
    value.append(reinterpret_cast<const char*>(targets.data()), targets.size_bytes());
    value.append(reinterpret_cast<const char*>(metas.data()), metas.size_bytes());
    return value;
}

}  // namespace low_latency_graphdb::storage
