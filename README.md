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

## Test

```sh
ctest --test-dir build --output-on-failure
```
