cmake_minimum_required(VERSION 3.20)
project(order-book CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Platform-specific compiler flags
if(MSVC)
    # MSVC: optimize for speed, suppress deprecation warnings for POSIX-like names
    set(CMAKE_CXX_FLAGS_RELEASE "/O2 /DNDEBUG")
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
    # GCC / Clang
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
endif()

include_directories(${PROJECT_SOURCE_DIR}/include)

# Core source files (shared between targets)
set(CORE_SOURCES
    src/order_book.cpp
    src/matching_engine.cpp
)

# Main binary (no networking — fully portable)
add_executable(order-book
    src/main.cpp
    src/csv_parser.cpp
    ${CORE_SOURCES}
)

# TCP server binary
add_executable(order-book-server
    src/tcp_server.cpp
    src/protocol.cpp
    ${CORE_SOURCES}
)

# TCP test client
add_executable(tcp-client
    tools/tcp_client.cpp
    src/protocol.cpp
)

# Benchmark binary (no networking — fully portable)
add_executable(benchmark
    bench/benchmark.cpp
    bench/order_generator.cpp
    ${CORE_SOURCES}
)
target_include_directories(benchmark PRIVATE ${PROJECT_SOURCE_DIR}/bench)

# Link Winsock on Windows for networking targets
if(WIN32)
    target_link_libraries(order-book-server ws2_32)
    target_link_libraries(tcp-client ws2_32)
endif()

# Tests (Google Test via FetchContent)
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(tests
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_object_pool.cpp
    ${CORE_SOURCES}
)
target_link_libraries(tests GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(tests)
