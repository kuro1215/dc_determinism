cmake_minimum_required(VERSION 3.20)
project(dc_determinism LANGUAGES C CXX)

# dc_determinism: the C++ determinism evaluator.
#
#   cmake -S src/dc_determinism -B build/dc_determinism/<platform> -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/dc_determinism/<platform>
#   ctest --test-dir build/dc_determinism/<platform> --output-on-failure
#
# scripts/build.ps1 and scripts/build.sh wrap these commands. Platforms: windows-amd64
# (MSVC), linux-amd64 and linux-arm64 (g++/clang++); `-DDC_DETERMINISM_TEST_TIMEOUT_SCALE=N` multiplies the CTest
# timeouts (slow hosts, for example under emulation).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DC_DETERMINISM_STATIC_RUNTIME "Link the C/C++ runtime statically (/MT, -static)" ON)
option(DC_DETERMINISM_BUILD_TESTS "Build the unit tests (CTest)" ON)

if(MSVC AND DC_DETERMINISM_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

get_filename_component(DC_DETERMINISM_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(DC_DETERMINISM_DEPS "${DC_DETERMINISM_REPO_ROOT}/src/vendor")
foreach(dependency nlohmann-json/single_include/nlohmann/json.hpp miniz/miniz.c miniz/miniz.h)
    if(NOT EXISTS "${DC_DETERMINISM_DEPS}/${dependency}")
        message(FATAL_ERROR "missing vendored dependency: ${DC_DETERMINISM_DEPS}/${dependency}")
    endif()
endforeach()

find_package(Threads REQUIRED)

# Warnings clean at /W4 and -Wall -Wextra.
add_library(dc_determinism_flags INTERFACE)
# Reproducible binaries (scripts/prebuild.ps1 -Check): no timestamps and no build paths in the output. These
# flags do not change the generated code.
if(MSVC)
    target_compile_options(dc_determinism_flags INTERFACE /W4 /utf-8 /permissive- /EHsc /Brepro)
    target_link_options(dc_determinism_flags INTERFACE /Brepro)
    target_compile_definitions(dc_determinism_flags INTERFACE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # No floating-point contraction: py_hypot and the log metrics must round exactly as CPython does.
    target_compile_options(dc_determinism_flags INTERFACE -Wall -Wextra -ffp-contract=off
                           "-ffile-prefix-map=${DC_DETERMINISM_REPO_ROOT}=.")
    if(DC_DETERMINISM_STATIC_RUNTIME)
        target_link_options(dc_determinism_flags INTERFACE -static)
    endif()
endif()

# miniz (single-file amalgamation, MIT), third-party: no warnings.
add_library(miniz STATIC "${DC_DETERMINISM_DEPS}/miniz/miniz.c")
target_include_directories(miniz SYSTEM PUBLIC "${DC_DETERMINISM_DEPS}/miniz")
if(MSVC)
    target_compile_options(miniz PRIVATE /W0)
else()
    target_compile_options(miniz PRIVATE -w)
endif()

if(WIN32)
    set(DC_DETERMINISM_PROCESS_SOURCES process/process_windows.cpp)
else()
    set(DC_DETERMINISM_PROCESS_SOURCES process/process_posix.cpp)
endif()

add_library(dc_determinism_core STATIC
    capture/archive.cpp
    capture/collector.cpp
    capture/compare_traj.cpp
    capture/dctr.cpp
    cli/argparse.cpp
    cli/commands.cpp
    cli/commands/benchmark.cpp
    cli/commands/capture.cpp
    cli/commands/capture_player.cpp
    cli/commands/check_recording.cpp
    cli/commands/compare_engines.cpp
    cli/commands/compare_player.cpp
    cli/commands/compare_pybind.cpp
    cli/commands/compare_simulator.cpp
    cli/commands/player_common.cpp
    cli/commands/prebuild.cpp
    cli/commands/prepare.cpp
    cli/commands/run_player.cpp
    cli/commands/run_simulator.cpp
    cli/commands/verify.cpp
    cli/spec.cpp
    common/environment.cpp
    common/fs.cpp
    common/log.cpp
    common/platform.cpp
    common/pymath.cpp
    common/scratch.cpp
    common/sha256.cpp
    common/tempdir.cpp
    common/text.cpp
    common/tree_hash.cpp
    corpus/capi_initial.cpp
    corpus/catalogue.cpp
    corpus/prepared.cpp
    corpus/pytext.cpp
    json/pyfloat.cpp
    json/pyparse.cpp
    json/writer.cpp
    logs/evaluator.cpp
    localbuild/local_build.cpp
    logs/log_compare.cpp
    player/binaries.cpp
    player/pdet.cpp
    player/result.cpp
    prebuild/binary_sections.cpp
    prebuild/prebuild.cpp
    process/process.cpp
    pybind/pybind.cpp
    sim/bench.cpp
    sim/compare.cpp
    sim/compilers.cpp
    sim/metrics.cpp
    sim/provenance.cpp
    sim/runner.cpp
    sim/wasm.cpp
    ${DC_DETERMINISM_PROCESS_SOURCES}
    zip/zip.cpp)
target_include_directories(dc_determinism_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(dc_determinism_core SYSTEM PUBLIC "${DC_DETERMINISM_DEPS}/nlohmann-json/single_include")
target_link_libraries(dc_determinism_core PUBLIC dc_determinism_flags miniz Threads::Threads)
if(WIN32)
    target_link_libraries(dc_determinism_core PUBLIC advapi32 shell32)
endif()

# Tool identity: the LF-normalised tree hash of this directory, embedded at build time.
add_executable(dc_determinism_treehash tools/treehash_main.cpp)
target_link_libraries(dc_determinism_treehash PRIVATE dc_determinism_core)

file(GLOB_RECURSE DC_DETERMINISM_SOURCE_TREE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*")
set(DC_DETERMINISM_IDENTITY_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/identity_generated.cpp")
add_custom_command(
    OUTPUT "${DC_DETERMINISM_IDENTITY_CPP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND dc_determinism_treehash --tree "${CMAKE_CURRENT_SOURCE_DIR}" --emit-cpp "${DC_DETERMINISM_IDENTITY_CPP}"
    DEPENDS dc_determinism_treehash ${DC_DETERMINISM_SOURCE_TREE}
    COMMENT "Computing the dc_determinism source tree hash"
    VERBATIM)
add_library(dc_determinism_identity STATIC common/identity.cpp "${DC_DETERMINISM_IDENTITY_CPP}")
target_link_libraries(dc_determinism_identity PUBLIC dc_determinism_core)

add_executable(dc_determinism main.cpp)
target_link_libraries(dc_determinism PRIVATE dc_determinism_core dc_determinism_identity)

if(DC_DETERMINISM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
