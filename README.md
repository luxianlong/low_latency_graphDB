# low_latency_graphDB

A low-latency graph database implemented in C++.

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
