include(FetchContent)

# --- xxHash Library ---
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG        v0.8.3
)
FetchContent_MakeAvailable(xxhash)
FetchContent_GetProperties(xxhash SOURCE_DIR XXHASH_SRC_DIR)

# --- Header files ---
file(GLOB_RECURSE DIGITALCURLING_LOADER_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.[Hh]"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

# --- Build Plugin Loader ---
if (DIGITALCURLING_PLUGIN_LOADER_SHARED)
    set(DIGITALCURLING_LOADER_TYPE SHARED)
else()
    set(DIGITALCURLING_LOADER_TYPE STATIC)
endif()

add_library(digitalcurling_plugin_loader ${DIGITALCURLING_LOADER_TYPE}
    "./src/plugins/loader.cpp"
    "./src/plugins/plugin_manager.cpp"
    "./src/plugins/detail/native_loader.cpp"
    "./src/plugins/detail/utf8_path.cpp"
    "./src/plugins/detail/plugin_resource.cpp"
    "./src/plugins/detail/v2_adapter.cpp"
    "./src/players/plugin_player.cpp"
    "./src/simulators/plugin_simulator.cpp"
)
add_library(digitalcurling::plugin_loader ALIAS digitalcurling_plugin_loader)
set_target_properties(digitalcurling_plugin_loader PROPERTIES EXPORT_NAME plugin_loader)

digitalcurling_apply_standard_settings(digitalcurling_plugin_loader)

target_compile_definitions(digitalcurling_plugin_loader PRIVATE "XXH_INLINE_ALL")
target_include_directories(digitalcurling_plugin_loader PRIVATE "${XXHASH_SRC_DIR}")

include(GenerateExportHeader)
set(DIGITALCURLING_LOADER_EXPORT_HEADER
    "${CMAKE_CURRENT_BINARY_DIR}/include/digitalcurling_plugin_loader_export.h"
)
generate_export_header(digitalcurling_plugin_loader
    BASE_NAME DIGITALCURLING_PLUGIN_LOADER
    EXPORT_MACRO_NAME DIGITALCURLING_LOADER_API
    EXPORT_FILE_NAME ${DIGITALCURLING_LOADER_EXPORT_HEADER}
)

target_sources(digitalcurling_plugin_loader PUBLIC
    FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${DIGITALCURLING_LOADER_HEADERS}

    FILE_SET generated_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/include"
        FILES ${DIGITALCURLING_LOADER_EXPORT_HEADER}
)
target_link_libraries(digitalcurling_plugin_loader
    PUBLIC digitalcurling::plugin_api ${CMAKE_DL_LIBS}
)
target_compile_definitions(digitalcurling_plugin_loader
    PRIVATE "DIGITALCURLING_LIBRARY_EXTENSION=\"${CMAKE_SHARED_MODULE_SUFFIX}\""
)

if(NOT DIGITALCURLING_PLUGIN_LOADER_SHARED)
    target_compile_definitions(digitalcurling_plugin_loader PUBLIC DIGITALCURLING_PLUGIN_LOADER_STATIC_DEFINE)
endif()

# --- Plugin Bundling Logic ---
if(DIGITALCURLING_BUNDLE_PLUGINS AND DIGITALCURLING_ACTIVE_PLUGIN_TARGETS)
    message(STATUS "Building plugin loader: Static Bundling Mode")

    # Target names inside $<LINK_LIBRARY:...> are not rewritten on export, so a
    # static loader needs the namespaced imported names in its install interface.
    foreach(_plugin_target IN LISTS DIGITALCURLING_ACTIVE_PLUGIN_TARGETS)
        target_link_libraries(digitalcurling_plugin_loader PRIVATE
            $<BUILD_INTERFACE:$<LINK_LIBRARY:WHOLE_ARCHIVE,${_plugin_target}>>
            $<INSTALL_INTERFACE:$<LINK_LIBRARY:WHOLE_ARCHIVE,digitalcurling::${_plugin_target}>>
        )
    endforeach()
else()
    message(STATUS "Building plugin loader: Dynamic Loading Mode")
endif()


# --- Tests ---
if(DIGITALCURLING_BUILD_TEST AND TARGET digitalcurling_test)
    if (DIGITALCURLING_BUNDLE_PLUGINS)
        target_sources(digitalcurling_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test/loader_test_base.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_static_loader.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_static_manager.cpp"
        )
    else()
        target_compile_definitions(digitalcurling_plugin_loader PRIVATE
            DIGITALCURLING_LOADER_TEST_SUPPORT
        )

        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures/long-name")
        add_dependencies(digitalcurling_test core04_fixture)
        target_compile_definitions(digitalcurling_test PRIVATE
            DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH="$<TARGET_FILE:core04_fixture>"
        )
        add_library(digitalcurling_invalid_v2_test_plugin MODULE "${CMAKE_CURRENT_SOURCE_DIR}/test/invalid_v2_plugin.cpp")
        target_link_libraries(digitalcurling_invalid_v2_test_plugin PRIVATE digitalcurling::plugin_api)
        target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_INVALID_V2_TEST_PLUGIN="$<TARGET_FILE:digitalcurling_invalid_v2_test_plugin>")
        add_dependencies(digitalcurling_test digitalcurling_invalid_v2_test_plugin)
        function(add_v2_fixture target mode macro)
            add_library(${target} MODULE "${CMAKE_CURRENT_SOURCE_DIR}/test/v2_validation_plugin.cpp")
            target_link_libraries(${target} PRIVATE digitalcurling::plugin_api)
            target_compile_definitions(${target} PRIVATE V2_VALIDATION_MODE=${mode})
            target_compile_definitions(digitalcurling_test PRIVATE ${macro}="$<TARGET_FILE:${target}>")
            add_dependencies(digitalcurling_test ${target})
        endfunction()
        add_v2_fixture(digitalcurling_v2_null_fixture 0 DIGITALCURLING_V2_NULL_PLUGIN)
        add_v2_fixture(digitalcurling_v2_missing_fixture 1 DIGITALCURLING_V2_MISSING_PLUGIN)
        add_v2_fixture(digitalcurling_v2_version_fixture 2 DIGITALCURLING_V2_VERSION_PLUGIN)
        add_v2_fixture(digitalcurling_v2_size_fixture 3 DIGITALCURLING_V2_SIZE_PLUGIN)
        add_v2_fixture(digitalcurling_v2_kind_fixture 4 DIGITALCURLING_V2_KIND_PLUGIN)
        add_v2_fixture(digitalcurling_v2_no_shot_a_fixture 5 DIGITALCURLING_V2_NO_SHOT_A_PLUGIN)
        add_v2_fixture(digitalcurling_v2_no_shot_b_fixture 6 DIGITALCURLING_V2_NO_SHOT_B_PLUGIN)
        add_v2_fixture(digitalcurling_v2_no_entrypoint_fixture 7 DIGITALCURLING_V2_NO_ENTRYPOINT_PLUGIN)
        add_v2_fixture(digitalcurling_v2_simulate_only_fixture 8 DIGITALCURLING_V2_SIMULATE_ONLY_PLUGIN)
        add_v2_fixture(digitalcurling_v2_simulate_only_shot_fixture 9 DIGITALCURLING_V2_SIMULATE_ONLY_SHOT_PLUGIN)
        add_v2_fixture(digitalcurling_v2_step_shot_fixture 10 DIGITALCURLING_V2_STEP_SHOT_PLUGIN)
        add_v2_fixture(digitalcurling_v2_partial_step_a_fixture 11 DIGITALCURLING_V2_PARTIAL_STEP_A_PLUGIN)
        add_v2_fixture(digitalcurling_v2_partial_step_b_fixture 12 DIGITALCURLING_V2_PARTIAL_STEP_B_PLUGIN)
        add_v2_fixture(digitalcurling_v2_simulator_size_fixture 13 DIGITALCURLING_V2_SIMULATOR_SIZE_PLUGIN)
        add_v2_fixture(digitalcurling_v2_player_size_fixture 14 DIGITALCURLING_V2_PLAYER_SIZE_PLUGIN)
        add_v2_fixture(digitalcurling_v2_player_table_size_fixture 15 DIGITALCURLING_V2_PLAYER_TABLE_SIZE_PLUGIN)
        add_v2_fixture(digitalcurling_v2_player_simulator_size_fixture 16 DIGITALCURLING_V2_PLAYER_SIMULATOR_SIZE_PLUGIN)
        add_v2_fixture(digitalcurling_v2_unterminated_name_fixture 17 DIGITALCURLING_V2_UNTERMINATED_NAME_PLUGIN)
        add_v2_fixture(digitalcurling_v2_invalid_utf8_name_fixture 18 DIGITALCURLING_V2_INVALID_UTF8_NAME_PLUGIN)
        add_v2_fixture(digitalcurling_v2_control_name_fixture 19 DIGITALCURLING_V2_CONTROL_NAME_PLUGIN)
        add_library(digitalcurling_v2_stale_layout_fixture MODULE "${CMAKE_CURRENT_SOURCE_DIR}/test/stale_v2_layout_plugin.cpp")
        target_link_libraries(digitalcurling_v2_stale_layout_fixture PRIVATE digitalcurling::plugin_api)
        target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_V2_STALE_LAYOUT_PLUGIN="$<TARGET_FILE:digitalcurling_v2_stale_layout_fixture>")
        add_dependencies(digitalcurling_test digitalcurling_v2_stale_layout_fixture)
        if(NOT DIGITALCURLING_PLUGIN_LOADER_SHARED)
            target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_TEST_V2_INTERNAL_LIFETIME=1)
        endif()
        target_sources(digitalcurling_test PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/test/dynamic_test_base.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/loader_test_base.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_dynamic_loader.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_dynamic_manager.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_plugin_function.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/test/test_v2_validation.cpp"
        )
    endif()

    target_sources(digitalcurling_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_handle_id.cpp"
    )

    target_link_libraries(digitalcurling_test PRIVATE digitalcurling::plugin_loader)
    target_include_directories(digitalcurling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test "${XXHASH_SRC_DIR}")
    target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_TEST_LIBRARY_PREFIX="${CMAKE_SHARED_MODULE_PREFIX}")
endif()


# --- Install rules ---
install(TARGETS digitalcurling_plugin_loader
    EXPORT DigitalCurlingTargets

    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}

    FILE_SET HEADERS
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILE_SET generated_headers
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
