// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PlayerPlayerStorage

#pragma once

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_storage.hpp"

#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#include "digitalcurling/players/plugin_player.hpp"
#include "digitalcurling/plugins/handle_id.hpp"

namespace digitalcurling::players {

/// @brief Player storage accessed through a plugin
class PluginPlayerStorage : public plugins::WrapperBase<digitalcurling::players::IPlayerStorage> {
public:
    /// @brief Constructor
    /// @param player_id Player ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    PluginPlayerStorage(std::string player_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
        : WrapperBase(std::move(player_id), std::move(instance_id), std::move(owner_resource))
          { }

    virtual std::unique_ptr<IPlayer> CreatePlayer() const override {
        return ExecuteResourceFunc<std::unique_ptr<IPlayer>>([&](auto resource) {
            auto id = resource->create_target.Execute(GetInstanceId());
            return std::make_unique<PluginPlayer>(GetPluginId(), id, resource);
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

    /// @brief Get the gender of the player saved in this storage
    ///
    /// Reads the top-level `gender` of the stored state, or `factory.gender` for
    /// players that keep their factory information under `factory`.
    ///
    /// @return Gender
    /// @throws std::runtime_error If the stored state has no gender
    virtual Gender GetGender() const override {
        auto const json = ToJson();
        if (auto const it = json.find("gender"); it != json.end()) {
            return it->get<Gender>();
        }
        if (auto const factory_it = json.find("factory"); factory_it != json.end() && factory_it->is_object()) {
            if (auto const it = factory_it->find("gender"); it != factory_it->end()) {
                return it->get<Gender>();
            }
        }
        throw std::runtime_error("The state of the player storage '" + GetPluginId() + "' has no gender.");
    }
};

} // namespace digitalcurling::players
