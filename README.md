# low_latency_graphDB

A low-latency graph database implemented in C++.

The current core includes a bounded query arena, a cache-line-sized hot-node
slab, a bounds-checked adjacency view, and an in-memory breadth-first traversal
engine. RocksDB integration will be added after the canonical storage schema
is stabilized.

## Requirements

- CMake 3.20 or newer
- A C++20-compatible compiler

## Build

```sh
cmake -S . -B build
cmake --build build
```

## Run

```sh
./build/low_latency_graphdb
```

The executable builds a small in-memory graph and runs a depth-limited BFS as
a smoke test. The traversal engine can currently be used with
`InMemoryGraph::add_edge` and `BreadthFirstSearch::run`.

## Latency benchmark

Correctness tests and performance tests have different purposes:

- `ctest` verifies traversal behavior and safety.
- `benchmark-cache-core` measures the cache-resident 64-byte hot path.
- The full BFS and hot-node benchmarks include query lifecycle overhead and
  are diagnostic workloads, not measurements of the cache-line target.

Build and run the deterministic parallel benchmark:

```sh
./build/low_latency_graphdb_latency_benchmark
```

Options include `--nodes`, `--degree`, `--queries`, `--depth`, and `--threads`.
The output reports p50, p95, p99, maximum, and mean per-query latency in
microseconds, plus aggregate queries per second.

For repeated access to one hot vertex, use:

```sh
./build/low_latency_graphdb_hot_node_benchmark \
  --degree 8 --queries 2000 --threads 4
```

This repeatedly runs a depth-1 query from vertex `0` after warming the graph,
and reports the same latency metrics.

To isolate the cache-resident 64-byte node scan itself:

```sh
./build/low_latency_graphdb_cache_core_benchmark 10000000 1
```

The first argument is scan iterations and the second is worker count. This
benchmark intentionally excludes graph lookup, query allocation, result
materialization, and thread scheduling inside each scan.

The same benchmark can be run through CMake:

```sh
cmake --build build --target benchmark-cache-core
```

## Test

```sh
ctest --test-dir build --output-on-failure
```
