// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginSimulatorStorage

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"

namespace digitalcurling::simulators {


/// @brief Simulator storage accessed through a plugin
class PluginSimulatorStorage : public plugins::WrapperBase<digitalcurling::simulators::ISimulatorStorage> {
public:
    /// @brief Constructor
    /// @param simulator_id Simulator ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    PluginSimulatorStorage(std::string simulator_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
        : WrapperBase(std::move(simulator_id), std::move(instance_id), std::move(owner_resource))
          {}

    virtual std::unique_ptr<ISimulator> CreateSimulator() const override {
        return ExecuteResourceFunc<std::unique_ptr<ISimulator>>([&](auto resource) -> std::unique_ptr<ISimulator> {
            auto id = resource->create_target.Execute(GetInstanceId());
            return CreatePluginSimulatorWrapper(GetPluginId(), id, resource);
        });
    }

    /// @brief Get the state of this storage as JSON
    /// @return JSON object
    virtual nlohmann::json ToJson() const override {
        return ExecuteResourceFunc<nlohmann::json>([&](auto resource) {
            auto res = resource->object_creator_get_state.Execute(GetInstanceId());
            return nlohmann::json::parse(res);
        });
    }
};

} // namespace digitalcurling::simulators
