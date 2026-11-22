// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IPlayerFactory

#pragma once

#include <memory>
#include <string>
#include "digitalcurling/common.hpp"
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"

namespace digitalcurling::players {


/// @brief Factory interface for constructing an IPlayer
///
/// Instances of `IPlayer` are created by `CreatePlayer()` on a derived class.
class IPlayerFactory : public plugins::IPluginObjectCreator, public plugins::FactoryHandle {
public:
    IPlayerFactory() = default;
    /// @brief Copy constructor
    IPlayerFactory(IPlayerFactory const&) = default;
    /// @brief Copy assignment operator
    IPlayerFactory & operator = (IPlayerFactory const&) = default;
    virtual ~IPlayerFactory() override = default;

    /// @brief Creates a player
    /// @return Created player
    virtual std::unique_ptr<IPlayer> CreatePlayer() const = 0;

    /// @brief Copies this instance
    /// @returns Copied instance
    virtual std::unique_ptr<IPlayerFactory> Clone() const = 0;

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
