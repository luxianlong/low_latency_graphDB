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

add_executable(low_latency_graphdb_hot_node_correctness_test
    tests/hot_node_correctness.cpp
)
target_link_libraries(low_latency_graphdb_hot_node_correctness_test
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

option(LLG_ENABLE_ROCKSDB "Build the RocksDB storage layer" ON)
if(LLG_ENABLE_ROCKSDB)
    find_package(RocksDB CONFIG QUIET)
    if(RocksDB_FOUND)
        set(LLG_ROCKSDB_TARGET RocksDB::rocksdb)
    else()
        include(FetchContent)
        set(WITH_TESTS OFF CACHE BOOL "" FORCE)
        set(WITH_TOOLS OFF CACHE BOOL "" FORCE)
        set(WITH_BENCHMARK_TOOLS OFF CACHE BOOL "" FORCE)
        set(WITH_CORE_TOOLS OFF CACHE BOOL "" FORCE)
        set(WITH_GFLAGS OFF CACHE BOOL "" FORCE)
        set(PORTABLE ON CACHE BOOL "" FORCE)
        set(FAIL_ON_WARNINGS OFF CACHE BOOL "" FORCE)
        set(ROCKSDB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(rocksdb
            GIT_REPOSITORY https://github.com/facebook/rocksdb.git
            GIT_TAG v9.7.4
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(rocksdb)
        set(LLG_ROCKSDB_TARGET rocksdb)
    endif()
    add_library(low_latency_graphdb_storage INTERFACE)
    target_link_libraries(low_latency_graphdb_storage
        INTERFACE low_latency_graphdb_core ${LLG_ROCKSDB_TARGET})
    find_package(Threads REQUIRED)
    target_link_libraries(low_latency_graphdb
        PRIVATE low_latency_graphdb_storage Threads::Threads)
    target_compile_definitions(low_latency_graphdb PRIVATE LLG_ENABLE_ROCKSDB=1)
    add_executable(low_latency_graphdb_rocksdb_bfs_benchmark
        benchmarks/rocksdb_bfs_benchmark.cpp)
    target_link_libraries(low_latency_graphdb_rocksdb_bfs_benchmark
        PRIVATE low_latency_graphdb_storage Threads::Threads)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(low_latency_graphdb_core_tests
        tests/core_tests.cpp
    )
    find_package(Threads REQUIRED)
    target_link_libraries(low_latency_graphdb_core_tests PRIVATE low_latency_graphdb_core Threads::Threads)
    add_test(NAME core COMMAND low_latency_graphdb_core_tests)
    add_test(NAME hot_node_correctness
        COMMAND low_latency_graphdb_hot_node_correctness_test)
    if(LLG_ENABLE_ROCKSDB)
        add_executable(low_latency_graphdb_rocksdb_tests tests/rocksdb_store_tests.cpp)
        target_link_libraries(low_latency_graphdb_rocksdb_tests
            PRIVATE low_latency_graphdb_storage)
        find_package(Threads REQUIRED)
        target_link_libraries(low_latency_graphdb_rocksdb_tests PRIVATE Threads::Threads)
        add_test(NAME rocksdb_store COMMAND low_latency_graphdb_rocksdb_tests)
    endif()
endif()
