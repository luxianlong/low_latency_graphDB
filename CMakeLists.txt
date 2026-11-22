cmake_minimum_required(VERSION 3.20)

project(low_latency_graphDB
    VERSION 0.1.0
    DESCRIPTION "A low-latency graph database in C++"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(low_latency_graphdb_core INTERFACE)
target_include_directories(low_latency_graphdb_core
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(low_latency_graphdb
    src/main.cpp
)
target_link_libraries(low_latency_graphdb PRIVATE low_latency_graphdb_core)

add_executable(low_latency_graphdb_latency_benchmark
    benchmarks/latency_benchmark.cpp
)
target_link_libraries(low_latency_graphdb_latency_benchmark
    PRIVATE low_latency_graphdb_core
)

add_executable(low_latency_graphdb_hot_node_benchmark
    benchmarks/hot_node_benchmark.cpp
)
target_link_libraries(low_latency_graphdb_hot_node_benchmark
    PRIVATE low_latency_graphdb_core
)

add_executable(low_latency_graphdb_cache_core_benchmark
    benchmarks/cache_core_benchmark.cpp
)
target_link_libraries(low_latency_graphdb_cache_core_benchmark
    PRIVATE low_latency_graphdb_core
)

add_executable(low_latency_graphdb_cache_resident_bfs_benchmark
    benchmarks/cache_resident_bfs_benchmark.cpp
)
target_link_libraries(low_latency_graphdb_cache_resident_bfs_benchmark
    PRIVATE low_latency_graphdb_core
)

add_custom_target(benchmark-cache-core
    COMMAND low_latency_graphdb_cache_core_benchmark 10000000 1
    DEPENDS low_latency_graphdb_cache_core_benchmark
    USES_TERMINAL
)

if(MSVC)
    target_compile_options(low_latency_graphdb_core INTERFACE /W4 /permissive-)
else()
    target_compile_options(low_latency_graphdb_core INTERFACE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(low_latency_graphdb_core_tests
        tests/core_tests.cpp
    )
    target_link_libraries(low_latency_graphdb_core_tests PRIVATE low_latency_graphdb_core)
    add_test(NAME core COMMAND low_latency_graphdb_core_tests)
endif()
