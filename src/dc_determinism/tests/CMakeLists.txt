# Unit tests (CTest). The C++ unit tests need no Python.
#
# DC_DETERMINISM_TEST_TIMEOUT_SCALE (cache variable, default 1, a positive integer) multiplies every TIMEOUT property below
# (use e.g. 20 under emulation). The fixed timeouts inside the tests are not scaled.

set(DC_DETERMINISM_TEST_TIMEOUT_SCALE 1 CACHE STRING "Multiplier of the CTest TIMEOUT properties (positive integer)")
if(NOT DC_DETERMINISM_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "DC_DETERMINISM_TEST_TIMEOUT_SCALE must be a positive integer, got: ${DC_DETERMINISM_TEST_TIMEOUT_SCALE}")
endif()
function(dc_determinism_scaled_timeout seconds result)
    math(EXPR scaled "${seconds} * ${DC_DETERMINISM_TEST_TIMEOUT_SCALE}")
    set(${result} ${scaled} PARENT_SCOPE)
endfunction()

foreach(name sha256 pyfloat json zip fs process cli corpus sim logs capture player localbuild wasm platform prebuild pybind)
    add_executable(test_${name} test_${name}.cpp)
    target_link_libraries(test_${name} PRIVATE dc_determinism_core dc_determinism_flags)
    target_include_directories(test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND test_${name})
endforeach()
target_compile_definitions(test_fs PRIVATE DC_DETERMINISM_REPO_ROOT_DIR="${DC_DETERMINISM_REPO_ROOT}")
target_compile_definitions(test_localbuild PRIVATE DC_DETERMINISM_REPO_ROOT_DIR="${DC_DETERMINISM_REPO_ROOT}")
target_compile_definitions(test_prebuild PRIVATE DC_DETERMINISM_REPO_ROOT_DIR="${DC_DETERMINISM_REPO_ROOT}")
dc_determinism_scaled_timeout(300 DC_DETERMINISM_UNIT_TIMEOUT)
set_tests_properties(localbuild process sim player wasm prebuild pybind PROPERTIES TIMEOUT ${DC_DETERMINISM_UNIT_TIMEOUT})
# The prebuild flows copy the vendored sources into a scratch directory for every fake rebuild and check (about 2.5
# minutes on Linux, 8 on Windows with the host, container and tool tests).
dc_determinism_scaled_timeout(1800 DC_DETERMINISM_PREBUILD_TIMEOUT)
set_tests_properties(prebuild PROPERTIES TIMEOUT ${DC_DETERMINISM_PREBUILD_TIMEOUT})
# test_sim, test_player and test_wasm drive the synthetic worker next to them; test_player drives the
# synthetic player; test_pybind drives the synthetic C API worker and the synthetic pybind worker (no Python).
add_executable(tool_fake_worker tool_fake_worker.cpp)
target_link_libraries(tool_fake_worker PRIVATE dc_determinism_core dc_determinism_flags)
add_executable(tool_fake_pybind tool_fake_pybind.cpp)
target_link_libraries(tool_fake_pybind PRIVATE dc_determinism_core dc_determinism_flags)
add_executable(tool_fake_player tool_fake_player.cpp)
target_link_libraries(tool_fake_player PRIVATE dc_determinism_flags)
add_dependencies(test_player tool_fake_player)
add_dependencies(test_sim tool_fake_worker)
add_dependencies(test_player tool_fake_worker)
add_dependencies(test_wasm tool_fake_worker)
add_dependencies(test_pybind tool_fake_worker tool_fake_pybind)

# The identity embedded in the binary: `dc_determinism --version` must print the tree hash of src/dc_determinism.
add_test(NAME version_matches_tree_hash
    COMMAND ${CMAKE_COMMAND}
        -DDC_DETERMINISM=$<TARGET_FILE:dc_determinism> -DTREEHASH=$<TARGET_FILE:dc_determinism_treehash>
        -DTREE=${CMAKE_CURRENT_SOURCE_DIR}/.. -P "${CMAKE_CURRENT_SOURCE_DIR}/check_version.cmake")

# The wrapper dc_determinism.sh (image / architecture selection, path normalizer), checked with fake docker and uname.
if(UNIX)
    find_program(DC_DETERMINISM_BASH bash)
    if(DC_DETERMINISM_BASH)
        add_test(NAME dc_determinism_sh
            COMMAND ${DC_DETERMINISM_BASH} "${DC_DETERMINISM_REPO_ROOT}/scripts/check.sh" "${DC_DETERMINISM_REPO_ROOT}/dc_determinism.sh")
    endif()
endif()
