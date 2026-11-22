# --- Build settings ---
if(DEFINED DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS)
    set(DIGITALCURLING_PLUGIN_OUTPUT_DIR "${DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS}")
else()
    set(DIGITALCURLING_PLUGIN_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
endif()

set(DIGITALCURLING_PLAYER_PREFIX "digitalcurling_player_")
set(DIGITALCURLING_PLAYER_TARGET_NAME_LIST "")

function(digitalcurling_add_player_plugin _name)
    cmake_parse_arguments(ARG "EXCLUDE_DEFAULT" "" "" ${ARGN})
    if (NOT ARG_EXCLUDE_DEFAULT AND DIGITALCURLING_BUILD_PLAYERS)
        set(_option_default ON)
    else()
        set(_option_default OFF)
    endif()

    string(TOUPPER ${_name} _upper_name)
    set(_option_name "DIGITALCURLING_BUILD_PLAYER_${_upper_name}")
    option(${_option_name} "Build player: ${_name}" ${_option_default})

    if(${_option_name})
        add_subdirectory("src/${_name}")
    endif()

    if(TARGET "${DIGITALCURLING_PLAYER_PREFIX}${_name}")
        set(_tmp_targets ${DIGITALCURLING_PLAYER_TARGET_NAME_LIST})
        list(APPEND _tmp_targets ${_name})

        set(DIGITALCURLING_PLAYER_TARGET_NAME_LIST "${_tmp_targets}" PARENT_SCOPE)
    endif()
endfunction()

# --- Build plugins ---
digitalcurling_add_player_plugin(identical EXCLUDE_DEFAULT)
digitalcurling_add_player_plugin(normal_dist EXCLUDE_DEFAULT)
digitalcurling_add_player_plugin(nd2)

# --- Check targets ---
if (NOT DIGITALCURLING_PLAYER_TARGET_NAME_LIST)
    message(WARNING "No player plugin targets specified. No player plugins will be built.")
    return()
endif()

string(JOIN ", " PLAYER_PLUGIN_TARGETS_STRING ${DIGITALCURLING_PLAYER_TARGET_NAME_LIST})
message(STATUS "Building player plugins: ${PLAYER_PLUGIN_TARGETS_STRING}")

list(TRANSFORM DIGITALCURLING_PLAYER_TARGET_NAME_LIST
     PREPEND ${DIGITALCURLING_PLAYER_PREFIX}
     OUTPUT_VARIABLE DIGITALCURLING_PLAYER_TARGETS
)
list(APPEND DIGITALCURLING_ACTIVE_PLUGIN_TARGETS ${DIGITALCURLING_PLAYER_TARGETS})
set(DIGITALCURLING_ACTIVE_PLUGIN_TARGETS ${DIGITALCURLING_ACTIVE_PLUGIN_TARGETS} PARENT_SCOPE)

set_target_properties(${DIGITALCURLING_PLAYER_TARGETS} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${DIGITALCURLING_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${DIGITALCURLING_PLUGIN_OUTPUT_DIR}"
)
foreach(_target IN LISTS DIGITALCURLING_PLAYER_TARGETS)
    digitalcurling_apply_standard_settings(${_target} HIDDEN_VISIBILITY)
    digitalcurling_apply_standard_settings(${_target}_obj HIDDEN_VISIBILITY)
endforeach()


# --- Tests ---
if(DIGITALCURLING_BUILD_TEST AND TARGET digitalcurling_test)
    target_sources(digitalcurling_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/common.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_docs_examples.cpp"
    )
    target_include_directories(digitalcurling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
    target_compile_definitions(digitalcurling_test PRIVATE
        DIGITALCURLING_TEST_DOCS_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/docs"
    )

    foreach(_target IN LISTS DIGITALCURLING_PLAYER_TARGET_NAME_LIST)
        target_sources(digitalcurling_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/test/test_${_target}.cpp")
        string(TOUPPER "${_target}" _upper_target)
        target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_TEST_DOCS_PLAYER_${_upper_target})
        # Bundled plugins are linked into the static loader with WHOLE_ARCHIVE.
        # Linking their object libraries here would add the same definitions to
        # the test executable a second time.
        if(NOT (DIGITALCURLING_BUILD_PLUGIN_LOADER AND DIGITALCURLING_BUNDLE_PLUGINS
                AND NOT DIGITALCURLING_PLUGIN_LOADER_SHARED))
            target_link_libraries(digitalcurling_test PRIVATE "${DIGITALCURLING_PLAYER_PREFIX}${_target}_obj")
        endif()
    endforeach()
endif()


# --- Install rules ---
digitalcurling_install_plugin_targets(${DIGITALCURLING_PLAYER_TARGETS})
