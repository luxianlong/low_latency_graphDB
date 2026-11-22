cmake_minimum_required(VERSION 3.20)

project(low_latency_graphDB
    VERSION 0.1.0
    DESCRIPTION "A low-latency graph database in C++"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(low_latency_graphdb
    src/main.cpp
)

target_include_directories(low_latency_graphdb
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(MSVC)
    target_compile_options(low_latency_graphdb PRIVATE /W4 /permissive-)
else()
    target_compile_options(low_latency_graphdb PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME startup COMMAND low_latency_graphdb)
endif()
