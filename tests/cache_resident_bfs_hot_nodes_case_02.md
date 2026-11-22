# Test Case 02: Depth-2 BFS Hot-Node Count Sweep

## Workload

- Graph nodes: 10,000,000
- Graph storage: 640 MiB of 64-byte node slabs
- Topology: unique-child tree
- Fanout: 8
- Query depth: 2
- Nodes visited per query: 73
- Hot-node counts: 10, 100, and 10,000
- Threads: 1 and 5
- Iterations: 200,000
- Queries per latency sample: 64
- Query starts: reproducible pseudo-random selection over the hot-node set

Command form:

```sh
./build/low_latency_graphdb_cache_resident_bfs_benchmark \
  0 <threads> 2 10000000 64 <hot_nodes>
```

## Results

| Hot nodes | Threads | Nodes visited | p50 ns | p95 ns | p99 ns | p50 ns/node | p95 ns/node | p99 ns/node |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 1 | 73 | 58 | 61 | 69 | 0.795 | 0.836 | 0.945 |
| 100 | 1 | 73 | 58 | 60 | 61 | 0.795 | 0.822 | 0.836 |
| 10,000 | 1 | 73 | 69 | 72 | 75 | 0.945 | 0.986 | 1.027 |
| 10 | 5 | 73 | 68 | 147 | 225 | 0.932 | 2.014 | 3.082 |
| 100 | 5 | 73 | 74 | 173 | 258 | 1.014 | 2.370 | 3.534 |
| 10,000 | 5 | 73 | 89 | 178 | 292 | 1.219 | 2.438 | 4.000 |

## Interpretation

For one thread, expanding the hot-node set from 10 to 10,000 increases p50
from 58 ns to 69 ns. The increase is now visible because the query order is
not sequential, although the graph layout remains regular.

For five threads, the median and tail increase gradually as the hot-node set
expands. The larger p99 at 10,000 hot nodes is consistent with a larger shared
working set and more cache/scheduling variation, but this test alone does not
separate cache-level effects from operating-system scheduling.
