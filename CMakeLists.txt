cmake_minimum_required(VERSION 3.15)
project(kvstore CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# Shared compile contract carried as an INTERFACE library so each executable
# inherits the same standard, warning flags, include path, and thread runtime
# with a single target_link_libraries line. Avoids the usual drift between
# multiple add_executable blocks each respecified by hand.
add_library(kvstore_common INTERFACE)
target_include_directories(kvstore_common INTERFACE include)
target_link_libraries(kvstore_common INTERFACE Threads::Threads)
target_compile_options(kvstore_common INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-O2>
)

# Executables. Source lists are extended as each component lands.
add_executable(kvstore-server
    src/main.cpp
    src/store.cpp
    src/protocol.cpp
    src/thread_pool.cpp
)
target_link_libraries(kvstore-server PRIVATE kvstore_common)

add_executable(kvstore-cli tests/client.cpp)
target_link_libraries(kvstore-cli PRIVATE kvstore_common)

add_executable(kvstore-bench tests/benchmark.cpp)
target_link_libraries(kvstore-bench PRIVATE kvstore_common)
