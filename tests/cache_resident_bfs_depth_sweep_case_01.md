# Test Case 01: Cache-Resident BFS Depth Sweep

## Workload

- Graph nodes: 10,000,000
- Graph storage: 640 MiB of 64-byte node slabs
- Topology: unique-child tree
- Fanout: 8
- Start node: `0`
- Threads: 1 and 5
- Query depths: 1, 2, 5, and 7
- Query-state batching: 64 queries/sample for depths 1 and 2; 1 query/sample
  for depths 5 and 7
- Runtime policy: depth-aware defaults targeting approximately one second of
  query work, with the five-thread depth-7 case below three seconds

Visited-node counts are:

```text
depth 1:          9
depth 2:         73
depth 5:     37,449
depth 7:  2,396,745
```

Command form:

```sh
./build/low_latency_graphdb_cache_resident_bfs_benchmark \
  0 <threads> <depth> 10000000
```

`iterations=0` selects the depth-aware iteration count.

## Results

| Depth | Threads | Nodes | p50 ns | p95 ns | p99 ns | p50 ns/node | p95 ns/node | p99 ns/node |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 9 | 7 | 7 | 8 | 0.778 | 0.778 | 0.889 |
| 1 | 5 | 9 | 7 | 21 | 53 | 0.778 | 2.333 | 5.889 |
| 2 | 1 | 73 | 52 | 54 | 54 | 0.712 | 0.740 | 0.740 |
| 2 | 5 | 73 | 62 | 158 | 234 | 0.849 | 2.164 | 3.205 |
| 5 | 1 | 37,449 | 37,000 | 37,958 | 43,084 | 0.988 | 1.014 | 1.150 |
| 5 | 5 | 37,449 | 41,708 | 84,500 | 85,167 | 1.114 | 2.256 | 2.274 |
| 7 | 1 | 2,396,745 | 10,284,541 | 13,643,375 | 14,531,167 | 4.291 | 5.692 | 6.063 |
| 7 | 5 | 2,396,745 | 24,400,250 | 29,920,000 | 33,386,375 | 10.181 | 12.484 | 13.930 |

## Interpretation

Depths 1 and 2 are consistent with a hot, cache-resident working set.
Depth 5 has a much larger active set than L1 or L2, but its similar median
latency per node indicates efficient sequential access and hardware prefetching;
it should not be classified as entirely L1/L2-resident.

Depth 7 touches approximately 146 MiB of node data and shows a substantial
increase in both median and tail latency, consistent with shared-cache
capacity pressure and memory traffic. These are latency-based observations,
not direct hardware-counter measurements.
