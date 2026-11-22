cmake_minimum_required(VERSION 3.26)

project(digitalcurling
    VERSION 4.1.0
    LANGUAGES CXX
)

include(GNUInstallDirs)

# --- Build options ---
option(DIGITALCURLING_BUILD_AS_WASM "Build simulator WASM modules for DigitalCurling system" OFF)

option(DIGITALCURLING_BUILD_PLAYERS "Build player plugins for DigitalCurling system" ON)
option(DIGITALCURLING_BUILD_SIMULATORS "Build simulator plugins for DigitalCurling system" ON)

option(DIGITALCURLING_BUILD_PLUGIN_LOADER "Build plugin loader for DigitalCurling system" ON)
option(DIGITALCURLING_PLUGIN_LOADER_SHARED "Build plugin loader as shared library" ${BUILD_SHARED_LIBS})
option(DIGITALCURLING_BUNDLE_PLUGINS "Statically link plugins into the loader library" OFF)
set(DIGITALCURLING_PLUGIN_OUTPUT_DIR "plugins" CACHE STRING "Output directory for plugins relative to build dir")
set(DIGITALCURLING_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/digitalcurling/plugins" CACHE STRING
    "Install directory for dynamically loaded plugins, relative to the install prefix")

option(DIGITALCURLING_BUILD_TEST "Build tests for DigitalCurling system" ${PROJECT_IS_TOP_LEVEL})
option(DIGITALCURLING_BUILD_DOCS "Build documents for DigitalCurling system" OFF)

# --- Build settings ---
if(DIGITALCURLING_BUILD_AS_WASM)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "DIGITALCURLING_BUILD_AS_WASM requires the Emscripten toolchain. Configure with emcmake or an Emscripten CMake toolchain file.")
    endif()
    if(DIGITALCURLING_BUILD_PLAYERS OR DIGITALCURLING_BUILD_PLUGIN_LOADER OR DIGITALCURLING_BUILD_TEST)
        message(WARNING "Building as WASM modules is only supported for the simulator.")
        set(DIGITALCURLING_BUILD_PLAYERS OFF)
        set(DIGITALCURLING_BUILD_PLUGIN_LOADER OFF)
        set(DIGITALCURLING_BUILD_TEST OFF)
    endif()
endif()

set(BUILD_SHARED_LIBS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
if(NOT DEFINED CMAKE_CXX_STANDARD_REQUIRED)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(DIGITALCURLING_ACTIVE_PLUGIN_TARGETS "")

if(DIGITALCURLING_BUILD_PLAYERS OR DIGITALCURLING_BUILD_SIMULATORS)
    if(NOT DIGITALCURLING_BUNDLE_PLUGINS)
        cmake_path(ABSOLUTE_PATH DIGITALCURLING_PLUGIN_OUTPUT_DIR
                BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
                OUTPUT_VARIABLE DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS)
        message(STATUS "Build plugins module to ${DIGITALCURLING_PLUGIN_OUTPUT_DIR_ABS}")
    endif()
else()
    if(DIGITALCURLING_BUILD_TEST AND DIGITALCURLING_BUILD_PLUGIN_LOADER)
        message(WARNING "If the simulator or player are not built, the plugin loader tests may not function properly.")
    endif()
endif()

if(DIGITALCURLING_BUNDLE_PLUGINS)
    set(DIGITALCURLING_PLUGIN_TYPE STATIC)
    add_compile_definitions(DIGITALCURLING_BUNDLE_PLUGINS)
else()
    set(DIGITALCURLING_PLUGIN_TYPE MODULE)
endif()

# Bundled plugins are static archives that a static loader pulls in through
# WHOLE_ARCHIVE; only then do they become part of the installed link interface.
# With a shared loader they are absorbed into the loader and not installed.
if(DIGITALCURLING_BUNDLE_PLUGINS AND DIGITALCURLING_BUILD_PLUGIN_LOADER AND NOT DIGITALCURLING_PLUGIN_LOADER_SHARED)
    set(DIGITALCURLING_EXPORT_BUNDLED_PLUGINS ON)
else()
    set(DIGITALCURLING_EXPORT_BUNDLED_PLUGINS OFF)
endif()

# Installs plugin targets: dynamic plugins go to DIGITALCURLING_PLUGIN_INSTALL_DIR
# without being exported, bundled static plugins are exported only when the
# loader is static (see above).
function(digitalcurling_install_plugin_targets)
    if(NOT ARGN)
        return()
    endif()
    if(DIGITALCURLING_BUNDLE_PLUGINS)
        if(DIGITALCURLING_EXPORT_BUNDLED_PLUGINS)
            install(TARGETS ${ARGN}
                EXPORT DigitalCurlingTargets
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
            )
        endif()
    else()
        install(TARGETS ${ARGN}
            LIBRARY DESTINATION "${DIGITALCURLING_PLUGIN_INSTALL_DIR}"
            RUNTIME DESTINATION "${DIGITALCURLING_PLUGIN_INSTALL_DIR}"
        )
    endif()
endfunction()

# --- Warnings and hardening ---
# Internal interface target; it is only linked through $<BUILD_INTERFACE:...>
# and never installed or exported. No floating-point flags (fast-math,
# fp-contract) may be added here: recorded games must replay bit-identically.
add_library(digitalcurling_build_flags INTERFACE)
set(DIGITALCURLING_LINKED_TARGET_TYPES "EXECUTABLE;SHARED_LIBRARY;MODULE_LIBRARY")
if(MSVC)
    target_compile_options(digitalcurling_build_flags INTERFACE /W4 /permissive- /sdl /guard:cf)
    # Static libraries are archived, not linked.
    target_link_options(digitalcurling_build_flags INTERFACE
        "$<$<IN_LIST:$<TARGET_PROPERTY:TYPE>,${DIGITALCURLING_LINKED_TARGET_TYPES}>:/guard:cf>"
    )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(digitalcurling_build_flags INTERFACE -Wall -Wextra -Wpedantic)
    if(NOT EMSCRIPTEN)
        target_compile_options(digitalcurling_build_flags INTERFACE -fstack-protector-strong)
        if(CMAKE_SYSTEM_NAME MATCHES "^(Linux|Android)$")
            target_compile_options(digitalcurling_build_flags INTERFACE
                -fstack-clash-protection
                # _FORTIFY_SOURCE needs optimization; some distributions predefine it.
                "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE>"
                "$<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-D_FORTIFY_SOURCE=2>"
            )
            target_link_options(digitalcurling_build_flags INTERFACE
                "$<$<IN_LIST:$<TARGET_PROPERTY:TYPE>,${DIGITALCURLING_LINKED_TARGET_TYPES}>:-Wl,-z,relro,-z,now,-z,noexecstack>"
            )
        endif()
    endif()
endif()

# Applies the common settings to a real (non-INTERFACE) target. Pass
# HIDDEN_VISIBILITY for plugin targets; the plugin loader keeps the default
# visibility and exports its API explicitly.
function(digitalcurling_apply_standard_settings target_name)
    cmake_parse_arguments(ARG "HIDDEN_VISIBILITY" "" "" ${ARGN})
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF)
    if(ARG_HIDDEN_VISIBILITY)
        set_target_properties(${target_name} PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
        )
    endif()
    target_link_libraries(${target_name} PRIVATE $<BUILD_INTERFACE:digitalcurling_build_flags>)
endfunction()

# --- Test preparations ---
if(DIGITALCURLING_BUILD_TEST)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.16.0
        SYSTEM
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    mark_as_advanced(
        BUILD_GMOCK BUILD_GTEST
        gmock_build_tests gtest_build_samples gtest_build_tests
        gtest_disable_pthreads gtest_force_shared_crt gtest_hide_internal_symbols
    )

    add_executable(digitalcurling_test)
    target_link_libraries(digitalcurling_test PRIVATE GTest::gtest_main GTest::gmock)
endif()


# --- Build libraries ---
add_subdirectory(src/core)
add_subdirectory(src/plugin-api)

if(DIGITALCURLING_BUILD_PLAYERS)
    add_subdirectory(src/player)
endif()

if(DIGITALCURLING_BUILD_SIMULATORS)
    add_subdirectory(src/simulator)
endif()

if(DIGITALCURLING_BUILD_PLUGIN_LOADER)
    add_subdirectory(src/plugin-loader)
endif()


# --- Build tests ---
if(DIGITALCURLING_BUILD_TEST)
    if(DIGITALCURLING_BUILD_PLUGIN_LOADER AND DIGITALCURLING_ACTIVE_PLUGIN_TARGETS AND NOT DIGITALCURLING_BUNDLE_PLUGINS)
        list(GET DIGITALCURLING_ACTIVE_PLUGIN_TARGETS 0 PLUGIN_TARGET)
        target_compile_definitions(digitalcurling_test PRIVATE
            DIGITALCURLING_TEST_DIR="$<TARGET_FILE_DIR:digitalcurling_test>"
            DIGITALCURLING_TEST_PLUGINS_DIR="$<TARGET_FILE_DIR:${PLUGIN_TARGET}>"
        )
        if(TARGET digitalcurling_simulator_fcv1)
            target_compile_definitions(digitalcurling_test PRIVATE
                DIGITALCURLING_TEST_SIMULATOR_PLUGIN_FILENAME="$<TARGET_FILE_NAME:digitalcurling_simulator_fcv1>"
            )
        endif()
        if(TARGET digitalcurling_player_nd2)
            target_compile_definitions(digitalcurling_test PRIVATE
                DIGITALCURLING_TEST_PLAYER_PLUGIN_FILENAME="$<TARGET_FILE_NAME:digitalcurling_player_nd2>"
            )
        endif()
    endif()

    include(GoogleTest)
    gtest_discover_tests(digitalcurling_test)
endif()

# --- Create documents ---
if(DIGITALCURLING_BUILD_DOCS)
    add_subdirectory(docs)
endif()


# --- Install rules ---
include(CMakePackageConfigHelpers)
set(DIGITALCURLING_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/DigitalCurling")

# Bundled static fcv1 links Box2D privately; the static archive then needs it
# again in the consumer.
if(DIGITALCURLING_EXPORT_BUNDLED_PLUGINS AND TARGET digitalcurling_simulator_fcv1)
    set(DIGITALCURLING_PACKAGE_NEEDS_BOX2D ON)
else()
    set(DIGITALCURLING_PACKAGE_NEEDS_BOX2D OFF)
endif()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DigitalCurlingConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/DigitalCurlingConfig.cmake"
    INSTALL_DESTINATION "${DIGITALCURLING_CMAKE_DIR}"
    PATH_VARS DIGITALCURLING_PLUGIN_INSTALL_DIR
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/DigitalCurlingConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/DigitalCurlingConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/DigitalCurlingConfigVersion.cmake"
    DESTINATION "${DIGITALCURLING_CMAKE_DIR}"
)
install(EXPORT DigitalCurlingTargets
    FILE DigitalCurlingTargets.cmake
    NAMESPACE digitalcurling::
    DESTINATION "${DIGITALCURLING_CMAKE_DIR}"
)
