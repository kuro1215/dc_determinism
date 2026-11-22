add_library(core04_fixture MODULE
    long_name_plugin.cpp
)
target_link_libraries(core04_fixture
    PRIVATE digitalcurling::plugin_api
)
set_target_properties(core04_fixture PROPERTIES
    SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
