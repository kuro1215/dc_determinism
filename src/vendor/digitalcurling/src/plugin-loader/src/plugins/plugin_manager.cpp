// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/detail/native_loader.hpp"
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "detail/utf8_path.hpp"

namespace digitalcurling::plugins {


#ifndef DIGITALCURLING_LIBRARY_EXTENSION
    #error "Build system must define DIGITALCURLING_LIBRARY_EXTENSION. Check CMakeLists.txt. for digitalcurling_plugin_loader."
#endif
const std::string LibraryExtension = DIGITALCURLING_LIBRARY_EXTENSION;

#ifdef DIGITALCURLING_BUNDLE_PLUGINS
    void RegisterStaticPluginV2(const DigitalCurling_PluginApiV2* api) {
        auto resource = digitalcurling::plugins::detail::PluginResource::CreateV2(api, std::nullopt);
        digitalcurling::plugins::PluginManager::GetInstance().RegisterPlugin(std::move(resource));
    }
#endif // DIGITALCURLING_BUNDLE_PLUGINS


PluginManager::~PluginManager() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    player_resources_.clear();
    simulator_resources_.clear();
}

PluginManager& PluginManager::GetInstance() {
    static PluginManager instance;
    return instance;
}

#ifndef DIGITALCURLING_PLUGIN_LOADER_DISABLE_DYNAMIC

namespace detail {

std::shared_ptr<PluginResource> LoadPluginResource(const std::filesystem::path& plugin_path) {
    std::filesystem::path resolved_path;
    auto exit = [&](const std::string& msg) -> std::shared_ptr<PluginResource> {
        const auto& diag_path = resolved_path.empty() ? plugin_path : resolved_path;
        throw plugin_error { DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN, "\"" + PathToUtf8(diag_path) + "\": " + msg };
    };

    try {
        if (plugin_path.empty() ||
            plugin_path.native().find(static_cast<std::filesystem::path::value_type>(0)) != std::filesystem::path::string_type::npos) {
            return exit("File does not exist or is not a regular file.");
        }

        resolved_path = std::filesystem::absolute(plugin_path);
        if (resolved_path.empty()) {
            return exit("File does not exist or is not a regular file.");
        }

        if (!std::filesystem::exists(resolved_path) || !std::filesystem::is_regular_file(resolved_path))
            return exit("File does not exist or is not a regular file.");
        if (!resolved_path.has_extension() || resolved_path.extension() != LibraryExtension)
            return exit("Invalid file extension. Expected " + LibraryExtension + ".");

        auto native = LoadNativePlugin(resolved_path);
        return PluginResource::CreateV2(native.get_plugin_api_v2(), std::move(native.handle));
    } catch (const plugin_error&) {
        throw;
    } catch (const std::filesystem::filesystem_error& e) {
        return exit("Filesystem operation failed (error code " + std::to_string(e.code().value()) + ").");
    } catch (const std::system_error& e) {
        return exit("Path conversion failed (error code " + std::to_string(e.code().value()) + ").");
    }
}

} // namespace detail

std::optional<PluginId> PluginManager::LoadPlugin(const std::filesystem::path& plugin_path, bool throw_exception) {
    try {
        auto resource = detail::LoadPluginResource(plugin_path);
        auto id = PluginId{ resource->GetType(), resource->GetName() };
        RegisterPlugin(std::move(resource));
        return id;
    } catch (const plugin_error&) {
        if (!throw_exception) return std::nullopt;
        throw;
    } catch (const std::exception&) {
        if (!throw_exception) return std::nullopt;
        throw;
    }
}
#endif // DIGITALCURLING_PLUGIN_LOADER_DISABLE_DYNAMIC

void PluginManager::RegisterPlugin(std::shared_ptr<detail::PluginResource> resource) {
    if (!resource) {
        throw plugin_error{DIGITALCURLING_ERR_INVALID_ARGUMENT, "Plugin resource is null."};
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto type = resource->GetType();
    const auto name = resource->GetName();
    if (type == PluginType::player) {
        auto player_resource = std::dynamic_pointer_cast<detail::PlayerPluginResource>(resource);
        if (player_resource) {
            auto it = player_resources_.find(name);
            if (it != player_resources_.end() && it->second) {
                if (it->second->IsSameModule(*player_resource)) {
                    return;
                }
                throw plugin_error{
                    DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN,
                    "Player plugin '" + name + "' is already registered by another module."
                };
            }
            player_resources_[name] = std::move(player_resource);
            return;
        }
    } else if (type == PluginType::simulator) {
        auto simulator_resource = std::dynamic_pointer_cast<detail::SimulatorPluginResource>(resource);
        if (simulator_resource) {
            auto it = simulator_resources_.find(name);
            if (it != simulator_resources_.end() && it->second) {
                if (it->second->IsSameModule(*simulator_resource)) {
                    return;
                }
                throw plugin_error{
                    DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN,
                    "Simulator plugin '" + name + "' is already registered by another module."
                };
            }
            simulator_resources_[name] = std::move(simulator_resource);
            return;
        }
    } else {
        throw plugin_error{DIGITALCURLING_ERR_INVALID_PLUGIN_TYPE, "The plugin is not supported."};
    }

    throw plugin_error {
        DIGITALCURLING_ERR_FAILED_TO_CAST,
        "Failed to load " + ToString(type) + " plugin: " + name
    };
}

std::vector<PluginId> PluginManager::GetLoadedPlugins(std::optional<PluginType> type) const {
    std::shared_lock lock(mutex_);
    std::vector<PluginId> ids;

    if (!type.has_value()) {
        ids.reserve(player_resources_.size() + simulator_resources_.size());
        for (const auto& pair : player_resources_) ids.push_back({PluginType::player, pair.first});
        for (const auto& pair : simulator_resources_) ids.push_back({PluginType::simulator, pair.first});
        return ids;
    } else if (type == PluginType::player) {
        for (const auto& pair : player_resources_) ids.push_back({PluginType::player, pair.first});
    } else if (type == PluginType::simulator) {
        for (const auto& pair : simulator_resources_) ids.push_back({PluginType::simulator, pair.first});
    } else {
        throw plugin_error{DIGITALCURLING_ERR_INVALID_PLUGIN_TYPE, "Plugin type is invalid."};
    }
    return ids;
}

// --- Player instance management ---
std::unique_ptr<players::PluginPlayerFactory> PluginManager::CreatePlayerFactory(const std::string& name) {
    auto res = GetPlayerPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Player plugin not found: " + name};
    }
    return std::make_unique<players::PluginPlayerFactory>(name, res->create_factory.Execute(nullptr), res);
}
std::unique_ptr<players::PluginPlayerFactory> PluginManager::CreatePlayerFactory(const nlohmann::json& json) {
    auto name = json.at("type").get<std::string>();
    auto res = GetPlayerPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Player plugin not found: " + name};
    }
    return std::make_unique<players::PluginPlayerFactory>(name, res->create_factory.Execute(json.dump().c_str()), res);
}
std::unique_ptr<players::PluginPlayerStorage> PluginManager::CreatePlayerStorage(const std::string& name) {
    auto res = GetPlayerPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Player plugin not found: " + name};
    }
    return std::make_unique<players::PluginPlayerStorage>(name, res->create_storage.Execute(nullptr), res);
}
std::unique_ptr<players::PluginPlayerStorage> PluginManager::CreatePlayerStorage(const nlohmann::json& json) {
    auto name = json.at("type").get<std::string>();
    auto res = GetPlayerPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Player plugin not found: " + name};
    }
    return std::make_unique<players::PluginPlayerStorage>(name, res->create_storage.Execute(json.dump().c_str()), res);
}

// --- Simulator instance management ---
std::unique_ptr<simulators::PluginSimulatorFactory> PluginManager::CreateSimulatorFactory(const std::string& name) {
    auto res = GetSimulatorPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin not found: " + name};
    }
    return std::make_unique<simulators::PluginSimulatorFactory>(name, res->create_factory.Execute(nullptr), res);
}
std::unique_ptr<simulators::PluginSimulatorFactory> PluginManager::CreateSimulatorFactory(const nlohmann::json& json) {
    auto name = json.at("type").get<std::string>();
    auto res = GetSimulatorPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin not found: " + name};
    }
    return std::make_unique<simulators::PluginSimulatorFactory>(name, res->create_factory.Execute(json.dump().c_str()), res);
}
std::unique_ptr<simulators::PluginSimulatorStorage> PluginManager::CreateSimulatorStorage(const std::string& name) {
    auto res = GetSimulatorPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin not found: " + name};
    }
    return std::make_unique<simulators::PluginSimulatorStorage>(name, res->create_storage.Execute(nullptr), res);
}
std::unique_ptr<simulators::PluginSimulatorStorage> PluginManager::CreateSimulatorStorage(const nlohmann::json& json) {
    auto name = json.at("type").get<std::string>();
    auto res = GetSimulatorPluginResource(name);
    if (!res) {
        throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin not found: " + name};
    }
    return std::make_unique<simulators::PluginSimulatorStorage>(name, res->create_storage.Execute(json.dump().c_str()), res);
}
DigitalCurling_PluginCapabilities PluginManager::GetSimulatorCapabilities(const std::string& name) const {
    auto resource = GetSimulatorPluginResource(name);
    if (!resource) throw plugin_error{DIGITALCURLING_ERR_PLUGIN_NOT_FOUND, "Simulator plugin is not loaded: " + name};
    return resource->GetCapabilities();
}

} // namespace digitalcurling::plugins
