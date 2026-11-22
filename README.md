# low_latency_graphDB

A low-latency graph database implemented in C++.

The current core includes a bounded query arena, a cache-line-sized hot-node
slab, and a bounds-checked adjacency view. RocksDB integration will be added
after the canonical storage schema is stabilized.

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

## Test

```sh
ctest --test-dir build --output-on-failure
```
