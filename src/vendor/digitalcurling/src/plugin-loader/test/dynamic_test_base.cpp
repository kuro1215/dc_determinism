// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <filesystem>
#include <iostream>
#include <string>
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "dynamic_test_base.hpp"

void digitalcurling::test::DynamicTestBase::SetUp() {
    std::filesystem::path base_plugin_dir = std::filesystem::path(DIGITALCURLING_TEST_DIR);

    const char* plugin_dir_env = std::getenv("DIGITALCURLING_PLUGIN_DIR");
    const std::filesystem::path env_plugin_dir = plugin_dir_env ? std::filesystem::path(plugin_dir_env) : std::filesystem::path{};
    if (plugin_dir_env && std::filesystem::exists(env_plugin_dir)) {
        base_plugin_dir = env_plugin_dir.lexically_normal();
        std::cout << "Using plugin directory from env (DIGITALCURLING_PLUGIN_DIR): ";
#ifdef DIGITALCURLING_TEST_PLUGINS_DIR
    } else if (std::filesystem::exists(std::filesystem::path(DIGITALCURLING_TEST_PLUGINS_DIR))) {
        base_plugin_dir = std::filesystem::path(DIGITALCURLING_TEST_PLUGINS_DIR).lexically_normal();
        std::cout << "Using plugin directory from macro (DIGITALCURLING_TEST_PLUGINS_DIR): ";
#endif
    } else {
        base_plugin_dir = (base_plugin_dir / "plugins").lexically_normal();
        std::cout << "Using plugin directory default path: ";
    }
    std::cout << base_plugin_dir.string() << std::endl;

#ifdef DIGITALCURLING_TEST_SIMULATOR_PLUGIN_FILENAME
    std::filesystem::path sim_path = base_plugin_dir / DIGITALCURLING_TEST_SIMULATOR_PLUGIN_FILENAME;
#else
    std::filesystem::path sim_path = base_plugin_dir /
        (std::string(DIGITALCURLING_TEST_LIBRARY_PREFIX) + "digitalcurling_simulator_" + std::string(kSimPluginName) + digitalcurling::plugins::LibraryExtension);
#endif
#ifdef DIGITALCURLING_TEST_PLAYER_PLUGIN_FILENAME
    std::filesystem::path player_path = base_plugin_dir / DIGITALCURLING_TEST_PLAYER_PLUGIN_FILENAME;
#else
    std::filesystem::path player_path = base_plugin_dir /
        (std::string(DIGITALCURLING_TEST_LIBRARY_PREFIX) + "digitalcurling_player_" + std::string(kPlayerPluginName) + digitalcurling::plugins::LibraryExtension);
#endif

    auto sim_res = TryLoad(sim_path, std::string(kSimPluginName));
    if (sim_res.has_value()) {
        std::cerr << "Warning (Setup): Failed to load simulator plugin: " << sim_res.value() << std::endl;
    } else {
        simulator_plugin_path_ = sim_path.string();
    }

    auto player_res = TryLoad(player_path, std::string(kPlayerPluginName));
    if (player_res.has_value()) {
        std::cerr << "Warning (Setup): Failed to load player plugin: " << player_res.value() << std::endl;
    } else {
        player_plugin_path_ = player_path.string();
    }
}
