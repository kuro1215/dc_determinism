# --- Box2D ---
set(DCSIM_FCV1_BOX2D_VERSION "v2.4.1")
include(FetchContent)
FetchContent_Declare(
    box2d
    GIT_REPOSITORY "https://github.com/erincatto/box2d.git"
    GIT_TAG        ${DCSIM_FCV1_BOX2D_VERSION}
    SYSTEM
)
# Box2D v2.4.1 declares cmake_minimum_required(VERSION 3.8). Raise it to 3.15
# on CMake >= 4 (older versions ignore the variable and accept 3.8), and make
# its option() calls honor the cache values below on every version.
set(CMAKE_POLICY_VERSION_MINIMUM 3.15)
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(box2d)

# --- Build plugin object ---
add_library(digitalcurling_simulator_fcv1_obj OBJECT
    "./simulator_fcv1.cpp"
    "./simulator_fcv1_factory.cpp"
    "./simulator_fcv1_storage.cpp"
)
target_include_directories(digitalcurling_simulator_fcv1_obj
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(digitalcurling_simulator_fcv1_obj
    PUBLIC  digitalcurling::plugin_api
    PRIVATE box2d
)

# --- Build plugin/module ---
if(DIGITALCURLING_BUILD_AS_WASM)
    add_executable(digitalcurling_simulator_fcv1
        $<TARGET_OBJECTS:digitalcurling_simulator_fcv1_obj>
        "./fcv1.cpp"
    )
    target_include_directories(digitalcurling_simulator_fcv1
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(digitalcurling_simulator_fcv1
        PUBLIC  digitalcurling::plugin_api
        PRIVATE box2d embind
    )
    target_link_options(digitalcurling_simulator_fcv1 PRIVATE
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=node,web"
        "--no-entry"
    )
    set_target_properties(digitalcurling_simulator_fcv1 PROPERTIES
        SUFFIX ".js"
    )
else()
    add_library(digitalcurling_simulator_fcv1 ${DIGITALCURLING_PLUGIN_TYPE}
        $<TARGET_OBJECTS:digitalcurling_simulator_fcv1_obj>
        "./fcv1.cpp"
    )
    target_include_directories(digitalcurling_simulator_fcv1
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(digitalcurling_simulator_fcv1
        PUBLIC  digitalcurling::plugin_api
        PRIVATE box2d
    )
endif()
