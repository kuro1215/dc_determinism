// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IPlayerStorage

#pragma once

#include <memory>
#include <string>
#include "digitalcurling/common.hpp"
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"

namespace digitalcurling::players {


/// @brief Storage interface for saving an IPlayer's state
///
/// When state is restored from storage using `IPlayer::Load()` or `IPlayerStorage::CreatePlayer()`,
/// the `IPlayer` can fully restore its internal state as saved by `IPlayer::Save()` or `IPlayer::CreateStorage()`.
/// In other words, calls to `IPlayer::Play()` after restoration give the same results as calls from the point at which the state was saved.
///
/// Storage corresponds to a particular `IPlayer` implementation, so saving or restoring requires matching IDs from `GetPlayerId()`.
class IPlayerStorage : public plugins::IPluginObjectCreator, public plugins::StorageHandle {
public:
    IPlayerStorage() = default;
    /// @brief Copy constructor
    IPlayerStorage(IPlayerStorage const&) = default;
    /// @brief Copy assignment operator
    IPlayerStorage & operator = (IPlayerStorage const&) = default;
    virtual ~IPlayerStorage() override = default;

    /// @brief Creates a player with the same state as this storage
    /// @returns Created player
    virtual std::unique_ptr<IPlayer> CreatePlayer() const = 0;

    /// @brief Gets the corresponding player ID
    ///
    /// Each player type has a distinct player ID.
    ///
    /// @returns Player ID
    virtual std::string GetPlayerId() const { return std::string(GetId()); }

    /// @brief Gets the player's gender
    /// @returns Player's gender
    virtual Gender GetGender() const = 0;

    virtual plugins::TargetHandle* Create() const override {
        return CreatePlayer().release();
    }
};

} // namespace digitalcurling::players
