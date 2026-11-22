# --- Build settings ---
if(DEFINED DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS)
    set(DIGITALCURLING_PLUGIN_OUTPUT_DIR "${DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS}")
else()
    set(DIGITALCURLING_PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

set(DIGITALCURLING_SIMULATOR_PREFIX "digitalcurling_simulator_")
set(DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST "")

function(digitalcurling_add_simulator_plugin _name)
    cmake_parse_arguments(ARG "EXCLUDE_DEFAULT" "" "" ${ARGN})
    if (NOT ARG_EXCLUDE_DEFAULT AND DIGITALCURLING_BUILD_SIMULATORS)
        set(_option_default ON)
    else()
        set(_option_default OFF)
    endif()

    string(TOUPPER "${_name}" _upper_name)
    set(_option_name "DIGITALCURLING_BUILD_SIMULATOR_${_upper_name}")
    option(${_option_name} "Build simulator: ${_name}" ${_option_default})

    if(${_option_name})
        add_subdirectory("src/${_name}")
    endif()

    if(TARGET "${DIGITALCURLING_SIMULATOR_PREFIX}${_name}")
        if(DIGITALCURLING_BUILD_AS_WASM)
            set_target_properties("${DIGITALCURLING_SIMULATOR_PREFIX}${_name}" PROPERTIES
                OUTPUT_NAME "${_name}"
            )
        endif()

        set(_tmp_targets ${DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST})
        list(APPEND _tmp_targets ${_name})

        set(DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST "${_tmp_targets}" PARENT_SCOPE)
    endif()
endfunction()

# --- Build plugins ---
digitalcurling_add_simulator_plugin(fcv1)
digitalcurling_add_simulator_plugin(fcv1plus)

# --- Check targets ---
if (NOT DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST)
    message(WARNING "No simulator plugin targets specified. No simulator plugins will be built.")
    return()
endif()

string(JOIN ", " SIMULATOR_PLUGIN_TARGETS_STRING ${DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST})
message(STATUS "Building simulator plugins: ${SIMULATOR_PLUGIN_TARGETS_STRING}")

list(TRANSFORM DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST
     PREPEND ${DIGITALCURLING_SIMULATOR_PREFIX}
     OUTPUT_VARIABLE DIGITALCURLING_SIMULATOR_TARGETS
)
list(APPEND DIGITALCURLING_ACTIVE_PLUGIN_TARGETS ${DIGITALCURLING_SIMULATOR_TARGETS})
set(DIGITALCURLING_ACTIVE_PLUGIN_TARGETS ${DIGITALCURLING_ACTIVE_PLUGIN_TARGETS} PARENT_SCOPE)

set_target_properties(${DIGITALCURLING_SIMULATOR_TARGETS} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${DIGITALCURLING_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${DIGITALCURLING_PLUGIN_OUTPUT_DIR}"
)
foreach(_target IN LISTS DIGITALCURLING_SIMULATOR_TARGETS)
    digitalcurling_apply_standard_settings(${_target} HIDDEN_VISIBILITY)
    digitalcurling_apply_standard_settings(${_target}_obj HIDDEN_VISIBILITY)
endforeach()


# --- Tests ---
if(DIGITALCURLING_BUILD_TEST AND TARGET digitalcurling_test)
    target_sources(digitalcurling_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/common.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_wasm_trajectory.cpp"
    )
    target_include_directories(digitalcurling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)

    foreach(_target IN LISTS DIGITALCURLING_SIMULATOR_TARGET_NAME_LIST)
        target_sources(digitalcurling_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test/test_${_target}.cpp")
        # The static loader already pulls each bundled plugin in through
        # WHOLE_ARCHIVE. Avoid adding its object files directly to the test.
        if(NOT (DIGITALCURLING_BUILD_PLUGIN_LOADER AND DIGITALCURLING_BUNDLE_PLUGINS
                AND NOT DIGITALCURLING_PLUGIN_LOADER_SHARED))
            target_link_libraries(digitalcurling_test PRIVATE "${DIGITALCURLING_SIMULATOR_PREFIX}${_target}_obj")
        endif()
    endforeach()

    # fcv1plus test headers use fpm. In bundled builds we intentionally do not
    # link the object library above, so retain its compile usage requirement
    # without reintroducing its object files.
    if(DIGITALCURLING_BUNDLE_PLUGINS AND NOT DIGITALCURLING_PLUGIN_LOADER_SHARED AND
       TARGET digitalcurling_simulator_fcv1plus_obj)
        target_link_libraries(digitalcurling_test PRIVATE $<BUILD_INTERFACE:fpm>)
    endif()

    # test_fcv1 includes the FCV1 headers (Box2D types) directly; take only the
    # include directories so the bundled plugin keeps its single Box2D copy.
    if(DIGITALCURLING_BUNDLE_PLUGINS AND NOT DIGITALCURLING_PLUGIN_LOADER_SHARED AND
       TARGET digitalcurling_simulator_fcv1_obj)
        target_include_directories(digitalcurling_test PRIVATE
            $<BUILD_INTERFACE:$<TARGET_PROPERTY:box2d,INTERFACE_INCLUDE_DIRECTORIES>>)
    endif()
endif()


# --- Install rules ---
digitalcurling_install_plugin_targets(${DIGITALCURLING_SIMULATOR_TARGETS})
