// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginPlayerFactory

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_factory.hpp"
#include "digitalcurling/players/plugin_player.hpp"
#include "digitalcurling/plugins/handle_id.hpp"

namespace digitalcurling::players {

/// @brief Player factory accessed through a plugin
class PluginPlayerFactory : public plugins::WrapperBase<digitalcurling::players::IPlayerFactory> {
public:
    /// @brief Constructor
    /// @param player_id Player ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    PluginPlayerFactory(std::string player_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
        : WrapperBase(std::move(player_id), std::move(instance_id), std::move(owner_resource))
          { }

    virtual std::unique_ptr<IPlayer> CreatePlayer() const override {
        return ExecuteResourceFunc<std::unique_ptr<IPlayer>>([&](auto resource) {
            auto id = resource->create_target.Execute(GetInstanceId());
            return std::make_unique<PluginPlayer>(GetPluginId(), id, resource);
        });
    }

    virtual std::unique_ptr<IPlayerFactory> Clone() const override {
        auto json = ToJson();
        return ExecuteResourceFunc<std::unique_ptr<IPlayerFactory>>([&](auto resource) {
            auto id = resource->create_factory.Execute(json.dump().c_str());
            return std::make_unique<PluginPlayerFactory>(GetPluginId(), id, resource);
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

    virtual Gender GetGender() const override {
        return ToJson().at("gender").get<Gender>();
    }

    /// @brief Set the gender
    /// @param new_gender New gender
    void SetGender(Gender new_gender) {
        auto json = (nlohmann::json {{ "gender", new_gender }}).dump();
        ExecuteResourceFunc<void>([&](auto resource) {
            resource->factory_set_state.Execute(GetInstanceId(), json);
        });
    }
};

} // namespace digitalcurling::players
