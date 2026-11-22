# dc_determinism overlay (not part of upstream fpm): the loader's FetchContent_MakeAvailable(fpm) adds this directory, and the
# simulator CMake files link the `fpm` target. Upstream's top-level CMakeLists.txt is not vendored (it can build
# tests and fetch GoogleTest); fpm itself is header-only.
cmake_minimum_required(VERSION 3.15)
project(fpm LANGUAGES CXX)

add_library(fpm INTERFACE)
target_include_directories(fpm SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
