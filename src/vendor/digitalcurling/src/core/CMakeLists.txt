# --- nlohmann_json ---
include(FetchContent)
# Install nlohmann_json next to DigitalCurling so that the installed package's
# find_dependency(nlohmann_json) resolves inside the same prefix. Has no effect
# when a system nlohmann_json is found instead of being fetched.
set(JSON_Install ON)
set(DIGITALCURLING_JSONLIB_VERSION "3.12.0")
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v${DIGITALCURLING_JSONLIB_VERSION}
    SYSTEM
    FIND_PACKAGE_ARGS ${DIGITALCURLING_JSONLIB_VERSION} QUIET
)
FetchContent_MakeAvailable(nlohmann_json)

# --- Header files ---
file(GLOB_RECURSE DIGITALCURLING_INCLUDE_FILES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
)

set(DIGITALCURLING_VERSION_INFO_HPP "include/digitalcurling/detail/version_info.hpp")
set(DIGITALCURLING_VERSION_INFO_HPP_OUT "${CMAKE_CURRENT_BINARY_DIR}/${DIGITALCURLING_VERSION_INFO_HPP}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/${DIGITALCURLING_VERSION_INFO_HPP}.in"
    "${DIGITALCURLING_VERSION_INFO_HPP_OUT}"
    @ONLY
)

# --- Build library ---
add_library(digitalcurling_core INTERFACE)
add_library(digitalcurling::core ALIAS digitalcurling_core)
set_target_properties(digitalcurling_core PROPERTIES EXPORT_NAME core)

target_sources(digitalcurling_core INTERFACE
    FILE_SET HEADERS
        BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
        FILES ${DIGITALCURLING_INCLUDE_FILES}

    FILE_SET generated_headers
        TYPE HEADERS
        BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/include"
        FILES "${DIGITALCURLING_VERSION_INFO_HPP_OUT}"
)
target_link_libraries(digitalcurling_core INTERFACE
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>
)
target_compile_options(digitalcurling_core INTERFACE
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/utf-8>
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>
)
target_compile_features(digitalcurling_core INTERFACE cxx_std_17)


# --- Tests ---
if(DIGITALCURLING_BUILD_TEST AND TARGET digitalcurling_test)
    target_sources(digitalcurling_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_json.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/test/test_version.cpp"
    )
    target_compile_definitions(digitalcurling_test PRIVATE
        DIGITALCURLING_TEST_EXPECTED_VERSION="${PROJECT_VERSION}"
    )
    target_link_libraries(digitalcurling_test PRIVATE digitalcurling::core)
    target_include_directories(digitalcurling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test)
endif()


# --- Install rules ---
install(TARGETS digitalcurling_core
    EXPORT DigitalCurlingTargets

    FILE_SET HEADERS
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILE_SET generated_headers
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
