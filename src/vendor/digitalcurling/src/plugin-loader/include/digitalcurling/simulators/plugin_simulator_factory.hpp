// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginSimulatorFactory

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"

namespace digitalcurling::simulators {


/// @brief Simulator factory accessed through a plugin
class PluginSimulatorFactory : public plugins::WrapperBase<digitalcurling::simulators::ISimulatorFactory> {
public:
    /// @brief Constructor
    /// @param simulator_id Simulator ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    PluginSimulatorFactory(std::string simulator_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
        : WrapperBase(std::move(simulator_id), std::move(instance_id), std::move(owner_resource))
          {}

    virtual std::unique_ptr<ISimulator> CreateSimulator() const override {
        return ExecuteResourceFunc<std::unique_ptr<ISimulator>>([&](auto resource) -> std::unique_ptr<ISimulator> {
            auto id = resource->create_target.Execute(GetInstanceId());
            return CreatePluginSimulatorWrapper(GetPluginId(), id, resource);
        });
    }

    virtual std::unique_ptr<ISimulatorFactory> Clone() const override {
        auto json = ToJson();
        return ExecuteResourceFunc<std::unique_ptr<ISimulatorFactory>>([&](auto resource) {
            auto id = resource->create_factory.Execute(json.dump().c_str());
            return std::make_unique<PluginSimulatorFactory>(GetPluginId(), id, resource);
        });
    }

    /// @brief Get the state of this factory as JSON
    /// @return JSON object
    virtual nlohmann::json ToJson() const override {
        return ExecuteResourceFunc<nlohmann::json>([&](auto resource) {
            auto res = resource->object_creator_get_state.Execute(GetInstanceId());
            return nlohmann::json::parse(res);
        });
    }
};

} // namespace digitalcurling::simulators
