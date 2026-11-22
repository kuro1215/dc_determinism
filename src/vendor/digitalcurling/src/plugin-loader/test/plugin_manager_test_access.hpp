// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include "digitalcurling/plugins/plugin_manager.hpp"

namespace digitalcurling::plugins::detail {

    struct PluginManagerTestAccess {
        static std::shared_ptr<PlayerPluginResource> GetPlayerResource(
            const PluginManager& manager, const std::string& name) {
            return manager.GetPlayerPluginResource(name);
        }

        static std::shared_ptr<SimulatorPluginResource> GetSimulatorResource(
            const PluginManager& manager, const std::string& name) {
            return manager.GetSimulatorPluginResource(name);
        }

        static void SetPlayerResource(
            PluginManager& manager,
            const std::string& name,
            std::shared_ptr<PlayerPluginResource> resource) {
            std::unique_lock lock(manager.mutex_);
            if (resource) {
                manager.player_resources_[name] = std::move(resource);
            } else {
                manager.player_resources_.erase(name);
            }
        }

        static void SetSimulatorResource(
            PluginManager& manager,
            const std::string& name,
            std::shared_ptr<SimulatorPluginResource> resource) {
            std::unique_lock lock(manager.mutex_);
            if (resource) {
                manager.simulator_resources_[name] = std::move(resource);
            } else {
                manager.simulator_resources_.erase(name);
            }
        }
    };

DIGITALCURLING_LOADER_API std::shared_ptr<PluginResource> CreateTestPluginResource(
    const std::filesystem::path& path, bool as_builtin = false);

} // namespace digitalcurling::plugins::detail
