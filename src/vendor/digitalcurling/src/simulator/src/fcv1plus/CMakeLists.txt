# --- fpm (header-only fixed-point library) ---
# Commit on the fpm default branch (not a tagged release). Keep in sync with
# src/player/src/nd2/CMakeLists.txt: FetchContent uses the first declaration.
set(DCSIM_FCV1PLUS_FPM_VERSION "b46537fe9697e1a598ac8a26f8ae43d8b286ac3f")
include(FetchContent)
FetchContent_Declare(
    fpm
    GIT_REPOSITORY "https://github.com/MikeLankamp/fpm.git"
    GIT_TAG        ${DCSIM_FCV1PLUS_FPM_VERSION}
    SYSTEM
)
FetchContent_MakeAvailable(fpm)

add_library(digitalcurling_simulator_fcv1plus_obj OBJECT
    "./simulator_fcv1plus.cpp"
    "./simulator_fcv1plus_factory.cpp"
    "./simulator_fcv1plus_storage.cpp"
    "./collision_solver.cpp"
)
if(DIGITALCURLING_BUILD_AS_WASM)
    target_compile_options(digitalcurling_simulator_fcv1plus_obj PRIVATE "-fwasm-exceptions")
endif()
target_include_directories(digitalcurling_simulator_fcv1plus_obj
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
# fpm is PUBLIC here (unlike on the final plugin target below) so that
# digitalcurling_test, which links this object library directly to compile
# test_fcv1plus.cpp, inherits fpm's include path too.
target_link_libraries(digitalcurling_simulator_fcv1plus_obj
    PUBLIC  digitalcurling::plugin_api
    PUBLIC  $<BUILD_INTERFACE:fpm>
)

if(DIGITALCURLING_BUILD_AS_WASM)
    add_executable(digitalcurling_simulator_fcv1plus
        $<TARGET_OBJECTS:digitalcurling_simulator_fcv1plus_obj>
        "./fcv1plus.cpp"
    )
    target_include_directories(digitalcurling_simulator_fcv1plus
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(digitalcurling_simulator_fcv1plus
        PUBLIC  digitalcurling::plugin_api
        PRIVATE $<BUILD_INTERFACE:fpm> embind
    )
    target_compile_options(digitalcurling_simulator_fcv1plus PRIVATE
        "-fwasm-exceptions"
    )
    target_link_options(digitalcurling_simulator_fcv1plus PRIVATE
        "-fwasm-exceptions"
        "-sMODULARIZE=1"
        "-sEXPORT_ES6=1"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sENVIRONMENT=node,web"
        "--no-entry"
    )
    set_target_properties(digitalcurling_simulator_fcv1plus PROPERTIES
        SUFFIX ".js"
    )
else()
    add_library(digitalcurling_simulator_fcv1plus ${DIGITALCURLING_PLUGIN_TYPE}
        $<TARGET_OBJECTS:digitalcurling_simulator_fcv1plus_obj>
        "./fcv1plus.cpp"
    )
    target_include_directories(digitalcurling_simulator_fcv1plus
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(digitalcurling_simulator_fcv1plus
        PUBLIC  digitalcurling::plugin_api
        PRIVATE $<BUILD_INTERFACE:fpm>
    )
endif()

# Test-only: test_fcv1plus.cpp directly compares CalculateShot() against a
# live fcv1 instance, which needs SimulatorFCV1's complete type and
# therefore Box2D's headers. fcv1's own CMakeLists.txt is intentionally left
# untouched; the include path is attached to the shared test binary here
# instead, entirely from this file. This is only wired up (and only
# advertised to the test via DIGITALCURLING_TEST_HAVE_FCV1) when fcv1 was
# actually built, so builds with DIGITALCURLING_BUILD_SIMULATOR_FCV1=OFF
# still succeed; test_fcv1plus.cpp #ifdefs the fcv1-comparison test on it.
if(DIGITALCURLING_BUILD_TEST AND TARGET digitalcurling_test AND
   DIGITALCURLING_BUILD_SIMULATOR_FCV1 AND TARGET box2d)
    target_link_libraries(digitalcurling_test PRIVATE box2d)
    target_compile_definitions(digitalcurling_test PRIVATE DIGITALCURLING_TEST_HAVE_FCV1=1)
endif()
