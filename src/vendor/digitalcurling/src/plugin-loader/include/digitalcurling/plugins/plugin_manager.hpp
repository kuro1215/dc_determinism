// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginManager

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/players/plugin_player_factory.hpp"
#include "digitalcurling/players/plugin_player_storage.hpp"
#include "digitalcurling/simulators/plugin_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"

#include "digitalcurling_plugin_loader_export.h"
#include "digitalcurling/plugins/detail/plugin_resource.hpp"

namespace digitalcurling::plugins {

namespace detail {
    class LoaderInternalAccessor;
    struct PluginManagerTestAccess;
#ifndef DIGITALCURLING_PLUGIN_LOADER_DISABLE_DYNAMIC
    std::shared_ptr<PluginResource> LoadPluginResource(const std::filesystem::path& plugin_path);
#endif
} // namespace digitalcurling::plugins::detail

/// @brief Shared library file extension
DIGITALCURLING_LOADER_API extern const std::string LibraryExtension;

/// @brief Plugin identifier
struct PluginId {
    /// @brief Plugin type
    PluginType type;
    /// @brief Plugin name
    std::string name;
};

/// @brief Class that manages plugins
class DIGITALCURLING_LOADER_API PluginManager {
public:
    /// @brief Get the singleton instance
    /// @return Reference to the singleton instance
    static PluginManager& GetInstance();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

#ifndef DIGITALCURLING_PLUGIN_LOADER_DISABLE_DYNAMIC
    /// @brief Load a plugin
    /// @param plugin_path File path of the plugin to load
    /// @param throw_exception Whether to throw an exception if loading fails
    /// @note Dependencies of a dynamic plugin are resolved from the plugin's own directory and the system
    /// directories only (not the current working directory or `PATH`), so they must be placed next to the plugin.
    /// Restricting which directories plugins are loaded from is the responsibility of the host application.
    std::optional<PluginId> LoadPlugin(const std::filesystem::path& plugin_path, bool throw_exception);
#endif

    /// @brief Register a plugin resource
    /// @param resource Shared pointer to the plugin resource to register
    void RegisterPlugin(std::shared_ptr<detail::PluginResource> resource);

    /// @brief Get information about loaded plugins
    /// @param type Plugin type (all types if omitted)
    /// @return List of loaded plugin IDs
    std::vector<PluginId> GetLoadedPlugins(std::optional<PluginType> type = std::nullopt) const;

    /// @brief Check whether the plugin is loaded
    /// @param type Plugin type
    /// @param name Plugin name
    /// @return `true` if the plugin is loaded
    inline bool IsPluginLoaded(PluginType type, const std::string& name) const {
        std::shared_lock lock(mutex_);
        if (type == PluginType::player) {
            return player_resources_.count(name) > 0;
        } else if (type == PluginType::simulator) {
            return simulator_resources_.count(name) > 0;
        }
        return false;
    }

    // --- Player instance management ---

    /// @brief Create a player factory
    /// @param name Plugin name
    std::unique_ptr<players::PluginPlayerFactory> CreatePlayerFactory(const std::string& name);
    /// @brief Create a player factory
    /// @param json JSON object used to construct the factory
    std::unique_ptr<players::PluginPlayerFactory> CreatePlayerFactory(const nlohmann::json& json);
    /// @brief Create a player storage
    /// @param name Plugin name
    std::unique_ptr<players::PluginPlayerStorage> CreatePlayerStorage(const std::string& name);
    /// @brief Create a player storage
    /// @param json JSON object used to construct the storage
    std::unique_ptr<players::PluginPlayerStorage> CreatePlayerStorage(const nlohmann::json& json);

    // --- Simulator instance management ---

    /// @brief Create a simulator factory
    /// @param name Plugin name
    std::unique_ptr<simulators::PluginSimulatorFactory> CreateSimulatorFactory(const std::string& name);
    /// @brief Create a simulator factory
    /// @param json JSON object used to construct the factory
    std::unique_ptr<simulators::PluginSimulatorFactory> CreateSimulatorFactory(const nlohmann::json& json);
    /// @brief Create a simulator storage
    /// @param name Plugin name
    std::unique_ptr<simulators::PluginSimulatorStorage> CreateSimulatorStorage(const std::string& name);
    /// @brief Create a simulator storage
    /// @param json JSON object used to construct the storage
    std::unique_ptr<simulators::PluginSimulatorStorage> CreateSimulatorStorage(const nlohmann::json& json);
    DigitalCurling_PluginCapabilities GetSimulatorCapabilities(const std::string& name) const;

private:
    PluginManager() = default;
    ~PluginManager();

    mutable std::shared_mutex mutex_;

    std::unordered_map<std::string, std::shared_ptr<detail::PlayerPluginResource>> player_resources_;
    std::unordered_map<std::string, std::shared_ptr<detail::SimulatorPluginResource>> simulator_resources_;

    inline std::shared_ptr<detail::PlayerPluginResource> GetPlayerPluginResource(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = player_resources_.find(name);
        return it != player_resources_.end() ? it->second : nullptr;
    }
    inline std::shared_ptr<detail::SimulatorPluginResource> GetSimulatorPluginResource(const std::string& name) const {
        std::shared_lock lock(mutex_);
        auto it = simulator_resources_.find(name);
        return it != simulator_resources_.end() ? it->second : nullptr;
    }

    friend class detail::LoaderInternalAccessor;
    friend struct detail::PluginManagerTestAccess;
};

} // namespace digitalcurling::plugins
