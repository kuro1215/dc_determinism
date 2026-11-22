// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginPlayer

#pragma once

#include <memory>
#include <string>
#include "digitalcurling/players/i_player.hpp"

#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/wrapper_base.hpp"

namespace digitalcurling::players {

/// @brief Player accessed through a plugin
class PluginPlayer : public plugins::WrapperBase<digitalcurling::players::IPlayer> {
public:
    /// @brief Constructor
    /// @param player_id Player ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    PluginPlayer(
        std::string player_id,
        digitalcurling::plugins::HandleId instance_id,
        std::weak_ptr<OwnerResource> owner_resource
    );

    virtual moves::Shot Play(moves::Shot const& shot) override;

    virtual Gender GetGender() const override;
    virtual std::unique_ptr<IPlayerFactory> GetFactory() const override;

    virtual std::unique_ptr<IPlayerStorage> CreateStorage() const override;
    virtual void Save(IPlayerStorage & storage) const override;
    virtual void Load(IPlayerStorage const& storage) override;
};

} // namespace digitalcurling::players
