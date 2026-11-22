# --- fpm ---
# Commit on the fpm default branch (not a tagged release). Keep in sync with
# src/simulator/src/fcv1plus/CMakeLists.txt.
set(DCPLY_ND2_FPM_VERSION "b46537fe9697e1a598ac8a26f8ae43d8b286ac3f")
include(FetchContent)
FetchContent_Declare(
    fpm
    GIT_REPOSITORY "https://github.com/MikeLankamp/fpm.git"
    GIT_TAG        ${DCPLY_ND2_FPM_VERSION}
    SYSTEM
)
FetchContent_MakeAvailable(fpm)

# --- Build plugin object ---
add_library(digitalcurling_player_nd2_obj OBJECT
    "./player_nd2.cpp"
    "./player_nd2_factory.cpp"
    "./player_nd2_storage.cpp"
    "./xoshiro256plusplus.cpp"
)
target_include_directories(digitalcurling_player_nd2_obj
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(digitalcurling_player_nd2_obj
    PUBLIC  digitalcurling::plugin_api
    PRIVATE fpm
)

# --- Build plugin ---
add_library(digitalcurling_player_nd2 ${DIGITALCURLING_PLUGIN_TYPE}
    $<TARGET_OBJECTS:digitalcurling_player_nd2_obj>
    "./nd2.cpp"
)
target_include_directories(digitalcurling_player_nd2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(digitalcurling_player_nd2
    PUBLIC digitalcurling::plugin_api
)
