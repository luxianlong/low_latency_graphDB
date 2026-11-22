#pragma once

#include "low_latency_graphdb/adjacency_view.hpp"
#include "low_latency_graphdb/in_memory_graph.hpp"
#include "low_latency_graphdb/storage_codec.hpp"

#include <rocksdb/convenience.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/table.h>
#include <rocksdb/cache.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace low_latency_graphdb::storage {

struct RocksDbGraphStoreOptions {
    std::size_t topo_block_cache_bytes{256ull << 20};
    std::size_t props_block_cache_bytes{16ull << 20};
    bool sync_writes{false};
    // Skips the WAL; only for bulk loads that call flush() before relying on data.
    bool disable_wal{false};
};

class StorageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns the RocksDB instance and the three column families from the design
// spec (cf_topo, cf_vertex_props, cf_edge_props).
class RocksDbGraphStore {
public:
    static constexpr std::size_t kMaxMaterializeBatchSize = 10'000;
    // Upper bound on simultaneously pinned adjacency pages.
    static constexpr std::size_t kMaxPinBatchSize = 4096;

    explicit RocksDbGraphStore(const std::string& path,
                               const RocksDbGraphStoreOptions& options = {})
        : sync_writes_(options.sync_writes), disable_wal_(options.disable_wal) {
        rocksdb::DBOptions db_options;
        db_options.create_if_missing = true;
        db_options.create_missing_column_families = true;
        db_options.IncreaseParallelism(2);
        db_options.max_open_files = -1;

        // Topology: point lookups by 8B key; keep it hot and uncompressed-ish.
        rocksdb::ColumnFamilyOptions topo_options;
        {
            rocksdb::BlockBasedTableOptions table;
            table.block_cache = rocksdb::NewLRUCache(options.topo_block_cache_bytes);
            table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
            // Index/filter live in the table reader (max_open_files = -1), so a
            // lookup skips two block-cache probes; data blocks still use the cache.
            table.cache_index_and_filter_blocks = false;
            table.data_block_index_type =
                rocksdb::BlockBasedTableOptions::kDataBlockBinaryAndHash;
            table.block_size = 4096;
            topo_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
            topo_options.compression = rocksdb::kNoCompression;
            topo_options.optimize_filters_for_hits = true;
        }

        // Cold properties: small cache quota, strongest available compression.
        rocksdb::ColumnFamilyOptions props_options;
        {
            rocksdb::BlockBasedTableOptions table;
            table.block_cache = rocksdb::NewLRUCache(options.props_block_cache_bytes);
            table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10));
            props_options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
            props_options.compression = best_compression();
        }

        std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
        descriptors.emplace_back(rocksdb::kDefaultColumnFamilyName,
                                 rocksdb::ColumnFamilyOptions());
        descriptors.emplace_back("cf_topo", topo_options);
        descriptors.emplace_back("cf_vertex_props", props_options);
        descriptors.emplace_back("cf_edge_props", props_options);

        std::vector<rocksdb::ColumnFamilyHandle*> handles;
        rocksdb::DB* raw = nullptr;
        check(rocksdb::DB::Open(db_options, path, descriptors, &handles, &raw), "open");
        db_.reset(raw);
        default_cf_ = handles[0];
        topo_cf_ = handles[1];
        vertex_props_cf_ = handles[2];
        edge_props_cf_ = handles[3];
    }

    RocksDbGraphStore(const RocksDbGraphStore&) = delete;
    RocksDbGraphStore& operator=(const RocksDbGraphStore&) = delete;

    ~RocksDbGraphStore() {
        for (auto* handle : {default_cf_, topo_cf_, vertex_props_cf_, edge_props_cf_}) {
            if (handle != nullptr) {
                db_->DestroyColumnFamilyHandle(handle);
            }
        }
    }

    // Atomic write of the topology row plus optional per-edge heavy payloads.
    // `edge_payloads` may be empty (no cf_edge_props writes) or match targets.
    void write_adjacency(std::uint64_t src, std::span<const std::uint64_t> targets,
                         std::span<const EdgeLightMeta> metas,
                         std::span<const std::string> edge_payloads = {}) {
        if (!edge_payloads.empty() && edge_payloads.size() != targets.size()) {
            throw std::invalid_argument("write_adjacency: edge payload size mismatch");
        }
        rocksdb::WriteBatch batch;
        const auto key = encode_vertex_key(src);
        const std::string value = encode_adjacency(targets, metas);
        check(batch.Put(topo_cf_, as_slice(key), value), "batch topo");
        for (std::size_t i = 0; i < edge_payloads.size(); ++i) {
            const auto edge_key = encode_edge_key(src, metas[i].edge_type, targets[i]);
            check(batch.Put(edge_props_cf_, as_slice(edge_key), edge_payloads[i]),
                  "batch edge props");
        }
        write(batch);
    }

    void write_vertex_props(std::uint64_t node_id, const std::string& payload) {
        rocksdb::WriteBatch batch;
        const auto key = encode_vertex_key(node_id);
        check(batch.Put(vertex_props_cf_, as_slice(key), payload), "batch vertex");
        write(batch);
    }

    // Pins one consistent view for a traversal and its late materialization.
    class Snapshot {
    public:
        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        Snapshot(Snapshot&& other) noexcept
            : db_(other.db_), snapshot_(other.snapshot_) {
            other.snapshot_ = nullptr;
        }
        ~Snapshot() {
            if (snapshot_ != nullptr) {
                db_->ReleaseSnapshot(snapshot_);
            }
        }
        [[nodiscard]] const rocksdb::Snapshot* get() const noexcept { return snapshot_; }

    private:
        friend class RocksDbGraphStore;
        Snapshot(rocksdb::DB* db, const rocksdb::Snapshot* snapshot)
            : db_(db), snapshot_(snapshot) {}
        rocksdb::DB* db_;
        const rocksdb::Snapshot* snapshot_;
    };

    [[nodiscard]] Snapshot snapshot() { return Snapshot(db_.get(), db_->GetSnapshot()); }

    // Holds the pinned block-cache page; the view points into it when aligned.
    class AdjacencyHandle {
    public:
        AdjacencyHandle() = default;
        [[nodiscard]] const ZeroCopyAdjView& view() const noexcept { return view_; }
        // True when the bytes alias a pinned block-cache page (no copy made).
        [[nodiscard]] bool is_pinned() const noexcept { return slice_.IsPinned(); }

    private:
        friend class RocksDbGraphStore;
        rocksdb::PinnableSlice slice_;
        ZeroCopyAdjView view_;
    };

    // Returns false when the vertex has no stored adjacency row. Not movable:
    // the view aliases the pinned slice.
    bool read_adjacency(std::uint64_t node_id, AdjacencyHandle& out,
                        const Snapshot* snapshot = nullptr) {
        rocksdb::ReadOptions read_options;
        if (snapshot != nullptr) {
            read_options.snapshot = snapshot->get();
        }
        out.slice_.Reset();
        const auto key = encode_vertex_key(node_id);
        const rocksdb::Status status =
            db_->Get(read_options, topo_cf_, as_slice(key), &out.slice_);
        if (status.IsNotFound()) {
            return false;
        }
        check(status, "get adjacency");
        out.view_.reset(out.slice_.data(), out.slice_.size());
        return true;
    }

    // Pins the adjacency rows of many vertices at once. Each found row's
    // block-cache page stays pinned (ref-counted, un-evictable) until the batch
    // is destroyed or reset, so keep batches bounded.
    class AdjacencyBatch {
    public:
        AdjacencyBatch() = default;
        AdjacencyBatch(const AdjacencyBatch&) = delete;
        AdjacencyBatch& operator=(const AdjacencyBatch&) = delete;

        [[nodiscard]] std::size_t size() const noexcept { return used_; }
        [[nodiscard]] std::uint64_t id(std::size_t i) const { return ids_.at(i); }
        [[nodiscard]] bool found(std::size_t i) const { return statuses_.at(i).ok(); }
        [[nodiscard]] bool is_pinned(std::size_t i) const {
            return statuses_.at(i).ok() && values_.at(i).IsPinned();
        }
        [[nodiscard]] std::size_t pinned_count() const noexcept {
            std::size_t n = 0;
            for (std::size_t i = 0; i < used_; ++i) {
                n += statuses_[i].ok() && values_[i].IsPinned() ? 1 : 0;
            }
            return n;
        }
        // Rebinds `view` to row i (must be found); valid while the batch lives.
        void bind(std::size_t i, ZeroCopyAdjView& view) const {
            if (!found(i)) {
                throw std::out_of_range("AdjacencyBatch: row not found");
            }
            view.reset(values_[i].data(), values_[i].size());
        }
        // Releases every pin but keeps buffers for reuse.
        void clear() {
            for (auto& value : values_) {
                value.Reset();
            }
            used_ = 0;
            ids_.clear();
        }

    private:
        friend class RocksDbGraphStore;
        std::size_t used_{0};
        std::vector<std::uint64_t> ids_;
        std::vector<rocksdb::PinnableSlice> values_;
        std::vector<rocksdb::Status> statuses_;
        std::vector<VertexKey> key_bufs_;
        std::vector<rocksdb::Slice> keys_;
    };

    // `ids` must be sorted ascending and unique (enables sorted MultiGet).
    void read_adjacency_batch(std::span<const std::uint64_t> ids, AdjacencyBatch& out,
                              const Snapshot* snapshot = nullptr) {
        if (ids.size() > kMaxPinBatchSize) {
            throw StorageError("pin batch exceeds kMaxPinBatchSize");
        }
        if (!std::is_sorted(ids.begin(), ids.end()) ||
            std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
            throw std::invalid_argument("read_adjacency_batch: ids must be sorted unique");
        }
        out.clear();
        const std::size_t n = ids.size();
        out.used_ = n;
        out.ids_.assign(ids.begin(), ids.end());
        if (out.values_.size() < n) {
            out.values_.resize(n);
            out.key_bufs_.resize(n);
            out.keys_.resize(n);
        }
        out.statuses_.assign(n, rocksdb::Status::OK());
        if (n == 0) {
            return;
        }
        for (std::size_t i = 0; i < n; ++i) {
            out.key_bufs_[i] = encode_vertex_key(ids[i]);
            out.keys_[i] = as_slice(out.key_bufs_[i]);
        }
        rocksdb::ReadOptions read_options;
        if (snapshot != nullptr) {
            read_options.snapshot = snapshot->get();
        }
        if (n < point_get_threshold_) {
            for (std::size_t i = 0; i < n; ++i) {
                out.statuses_[i] =
                    db_->Get(read_options, topo_cf_, out.keys_[i], &out.values_[i]);
            }
        } else {
            db_->MultiGet(read_options, topo_cf_, n, out.keys_.data(), out.values_.data(),
                          out.statuses_.data(), /*sorted_input=*/true);
        }
        for (const auto& status : out.statuses_) {
            if (!status.ok() && !status.IsNotFound()) {
                check(status, "read adjacency");
            }
        }
    }

    // Batches smaller than this use plain Get (cheaper than MultiGet setup).
    void set_point_get_threshold(std::size_t n) noexcept { point_get_threshold_ = n; }

    // Deduplicates + sorts candidates, then one batched MultiGet. Missing keys
    // yield std::nullopt. Throws StorageError past kMaxMaterializeBatchSize.
    std::vector<std::optional<std::string>> fetch_vertex_props(
        std::vector<std::uint64_t> candidate_ids, const Snapshot* snapshot = nullptr) {
        std::sort(candidate_ids.begin(), candidate_ids.end());
        candidate_ids.erase(std::unique(candidate_ids.begin(), candidate_ids.end()),
                            candidate_ids.end());
        if (candidate_ids.size() > kMaxMaterializeBatchSize) {
            throw StorageError("materialization batch exceeds kMaxMaterializeBatchSize");
        }

        const std::size_t n = candidate_ids.size();
        std::vector<VertexKey> key_bufs(n);
        std::vector<rocksdb::Slice> keys(n);
        for (std::size_t i = 0; i < n; ++i) {
            key_bufs[i] = encode_vertex_key(candidate_ids[i]);
            keys[i] = as_slice(key_bufs[i]);
        }

        rocksdb::ReadOptions read_options;
        if (snapshot != nullptr) {
            read_options.snapshot = snapshot->get();
        }
        std::vector<rocksdb::PinnableSlice> values(n);
        std::vector<rocksdb::Status> statuses(n);
        if (n > 0) {
            db_->MultiGet(read_options, vertex_props_cf_, n, keys.data(),
                          values.data(), statuses.data(), /*sorted_input=*/true);
        }

        std::vector<std::optional<std::string>> result(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (statuses[i].ok()) {
                result[i].emplace(values[i].data(), values[i].size());
            } else if (!statuses[i].IsNotFound()) {
                check(statuses[i], "multiget vertex props");
            }
        }
        return result;
    }

    // Visits every cf_topo row in ascending vertex-ID order.
    template <typename Visitor>
    void for_each_adjacency(Visitor&& visitor) {
        std::unique_ptr<rocksdb::Iterator> it(
            db_->NewIterator(rocksdb::ReadOptions(), topo_cf_));
        ZeroCopyAdjView view;
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            if (it->key().size() != sizeof(std::uint64_t)) {
                throw StorageError("cf_topo: malformed key");
            }
            std::uint64_t be{};
            std::memcpy(&be, it->key().data(), sizeof(be));
            view.reset(it->value().data(), it->value().size());
            visitor(to_big_endian(be), static_cast<const ZeroCopyAdjView&>(view));
        }
        check(it->status(), "iterate cf_topo");
    }

    // Warms an in-memory graph from every cf_topo row.
    void load_into(InMemoryGraph& graph) {
        for_each_adjacency([&](std::uint64_t src, const ZeroCopyAdjView& view) {
            graph.add_vertex(src);
            for (std::size_t i = 0; i < view.size(); ++i) {
                graph.add_edge(src, view.target(i));
            }
        });
    }

    void flush() {
        rocksdb::FlushOptions options;
        for (auto* cf : {topo_cf_, vertex_props_cf_, edge_props_cf_}) {
            check(db_->Flush(options, cf), "flush");
        }
    }

    [[nodiscard]] std::optional<std::string> read_edge_props(
        std::uint64_t src, std::uint16_t edge_type, std::uint64_t dst) {
        std::string value;
        const auto key = encode_edge_key(src, edge_type, dst);
        const auto status =
            db_->Get(rocksdb::ReadOptions(), edge_props_cf_, as_slice(key), &value);
        if (status.IsNotFound()) {
            return std::nullopt;
        }
        check(status, "get edge props");
        return value;
    }

private:
    template <typename Key>
    static rocksdb::Slice as_slice(const Key& key) noexcept {
        return rocksdb::Slice(key.data(), key.size());
    }

    static rocksdb::CompressionType best_compression() {
        const auto supported = rocksdb::GetSupportedCompressions();
        for (auto type : {rocksdb::kZSTD, rocksdb::kLZ4Compression,
                          rocksdb::kSnappyCompression}) {
            if (std::find(supported.begin(), supported.end(), type) != supported.end()) {
                return type;
            }
        }
        return rocksdb::kNoCompression;
    }

    static void check(const rocksdb::Status& status, const char* what) {
        if (!status.ok()) {
            throw StorageError(std::string("RocksDB ") + what + ": " + status.ToString());
        }
    }

    void write(rocksdb::WriteBatch& batch) {
        rocksdb::WriteOptions options;
        options.sync = sync_writes_;
        options.disableWAL = disable_wal_;
        check(db_->Write(options, &batch), "write");
    }

    std::unique_ptr<rocksdb::DB> db_;
    bool sync_writes_{false};
    bool disable_wal_{false};
    std::size_t point_get_threshold_{0};
    rocksdb::ColumnFamilyHandle* default_cf_{nullptr};
    rocksdb::ColumnFamilyHandle* topo_cf_{nullptr};
    rocksdb::ColumnFamilyHandle* vertex_props_cf_{nullptr};
    rocksdb::ColumnFamilyHandle* edge_props_cf_{nullptr};
};

}  // namespace low_latency_graphdb::storage
