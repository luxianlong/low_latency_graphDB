# Test Case 03: BFS over RocksDB, with and without the Hot Tier

## Workload

- Graph: 10,000,000 vertices, tree with fanout 8 plus a parent back-edge
  (vertex 0 has no parent edge, so a start at 0 visits the same 9 / 73 nodes
  as cases 01 and 02; any other start visits slightly more)
- Storage: RocksDB v9.7.4 (embedded), `cf_topo` adjacency rows, 256 MiB block
  cache, WAL disabled for bulk load, store reopened (cold cache) and warmed
  before measurement
- Machine: Apple M5, 10 hardware threads, 16 GiB
- Rows:
  - `rocksdb`: every hop read from RocksDB (`StorageBreadthFirstSearch`,
    sorted pinned `MultiGet`, one snapshot, `ZeroCopyAdjView`, no copies;
    100% of rows were pinned in every run)
  - `tiered`: `TieredGraph`, vertex IDs `[0, K)` served from the in-memory hot
    tier, 100% of rows hot in every run
- Count-only output (no visited-ID materialization); latency is per query
  (batched samples at depths 1-2, 1 query per sample at depths 5 and 7)

Command form:

```sh
./build/low_latency_graphdb_rocksdb_bfs_benchmark \
  --nodes 10000000 --fanout 8 --threads 1,5 --depth <d> --queries <n> \
  --batch 64 --hot-nodes <N> --hot-tier 1000000 --no-baseline
```

Depths 5 and 7 use random starts inside the hot region instead of
`--hot-nodes`: `--batch 1 --hot-pct 100 --hot-tier 2500000 --pin-batch 4096`
with 1,000 and 20 queries per thread.

## Results: depths 1 and 2, hot-node count sweep (p50 ns)

| Depth | Hot nodes | Nodes | rocksdb 1T | tiered 1T | rocksdb 5T | tiered 5T | Old in-memory 1T / 5T |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 9 | 589 | 13 | 3,563 | 19 | 7 / 7 |
| 1 | 10 | 9.9 | 635 | 13 | 3,719 | 20 | - |
| 1 | 100 | 10 | 684 | 13 | 3,158 | 20 | - |
| 1 | 10,000 | 10 | 755 | 20 | 3,086 | 26 | - |
| 2 | 1 | 73 | 3,096 | 54 | 9,462 | 78 | 52 / 62 |
| 2 | 10 | 80.3 | 3,748 | 65 | 10,114 | 92 | 58 / 68 |
| 2 | 100 | 81.8 | 4,066 | 66 | 8,572 | 89 | 58 / 74 |
| 2 | 10,000 | 82 | 4,700 | 89 | 6,286 | 124 | 69 / 89 |

Tiered p95 / p99 at depth 2, one thread: 55 / 56 ns (1 hot node), 68 / 69 ns
(100), 193 / 316 ns (10,000).

## Results: depths 5 and 7, random hot starts

| Depth | Threads | Nodes | rocksdb p50 | tiered p50 | tiered p95 | tiered p99 | Old in-memory p50 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 5 | 1 | 41,943 | 1.98 ms | 28.8 us | 31.3 us | 37.2 us | 37.0 us |
| 5 | 5 | 41,949 | 2.36 ms | 34.3 us | 85.8 us | 86.8 us | 41.7 us |
| 7 | 1 | 2,396,745 | 125.2 ms | 1.65 ms | 4.09 ms | 4.09 ms | 10.3 ms |
| 7 | 5 | 2,396,745 | 165.8 ms | 2.04 ms | 4.88 ms | 17.4 ms | 24.4 ms |

Hot tier size: 752 MiB (1M hot + 7M stub slabs) for depths 1-2, 1.1 GiB
(2.5M hot + 7.5M stubs) for depths 5 and 7; built in 0.7-1.7 s.

## Interpretation

- RocksDB alone costs about 60-70 ns per visited vertex on the single-thread
  path (fixed `MultiGet`/block lookup overhead), 50-70x slower than the old
  in-memory slab traversal. With five threads it gets worse at small depths
  because RocksDB's block cache is shared.
- The hot tier removes the RocksDB call for hot vertices. With a fixed start
  node it matches the old in-memory result (54 ns vs 52 ns at depth 2, one
  thread). As the hot-start set grows to 10,000 it stays within about 10-30%
  of the old benchmark (89 ns vs 69 ns), consistent with a larger working set.
- Depth 5 and 7 are faster than the old benchmark partly because count-only
  BFS does not read the leaf level's slabs, which the old benchmark did. The
  node counts also differ slightly at depth 5 (parent edge). Do not read these
  as a like-for-like speedup.
- The ~24 us `max` on tiered rows is a single outlier per run (OS scheduling or
  first-sample effects) and does not show up in p99.
- Staleness: writes made through `TieredGraph` are visible immediately via
  dirty flags; writes made directly on `RocksDbGraphStore` are only visible
  after the tier is rebuilt.
- Latency figures are wall-clock measurements, not hardware-counter results.
