// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IPlayer

#pragma once

#include <string>
#include <memory>
#include "digitalcurling/vector2.hpp"
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"

namespace digitalcurling::players {

class IPlayerFactory;
class IPlayerStorage;

/// @brief Interface for representing player shot variation
///
/// Adds variation to a player's shot using random numbers or similar methods.
///
/// `IPlayerStorage` can save and restore the internal state.
/// Use `IPlayer::CreateStorage()` or `IPlayer::Save()` to save state,
/// and `IPlayerStorage::CreatePlayer()` or `IPlayer::Load()` to restore it.
///
/// @note
/// An `IPlayer` instance is not thread-safe. Callers must serialize all calls on one instance.
/// Distinct instances may be used concurrently.
class IPlayer : public plugins::IPluginObject, public plugins::PlayerHandle {
public:
    IPlayer() = default;
    /// @brief Copy constructor
    IPlayer(IPlayer const&) = default;
    /// @brief Copy assignment operator
    IPlayer & operator = (IPlayer const&) = default;
    virtual ~IPlayer() override = default;

    /// @brief Plays a shot
    /// @param[in] shot Ideal shot
    /// @returns Shot played by the player
    virtual moves::Shot Play(moves::Shot const& shot) = 0;

    /// @brief Gets the player ID
    ///
    /// Each player type has a distinct player ID.
    ///
    /// @returns Player ID
    virtual std::string GetPlayerId() const { return std::string(GetId()); }

    /// @brief Gets the player's gender
    /// @returns Gender
    virtual Gender GetGender() const = 0;

    /// @brief Gets a copy of the factory
    ///
    /// The returned factory may contain more information than the `IPlayerFactory` instance
    /// that created this `IPlayer`. For example, its seed may have been filled in
    /// even when the original factory did not specify one.
    ///
    /// Creating an `IPlayer` from the returned factory yields the same initial conditions.
    ///
    /// @returns Copy of the factory (it stays valid after later calls)
    virtual std::unique_ptr<IPlayerFactory> GetFactory() const = 0;

    /// @brief Creates storage
    /// @returns Newly created storage
    virtual std::unique_ptr<IPlayerStorage> CreateStorage() const = 0;

    /// @brief Saves the state to storage
    /// @param[out] storage Storage
    virtual void Save(IPlayerStorage & storage) const = 0;

    /// @brief Restores the state from storage
    /// @param[in] storage Storage
    virtual void Load(IPlayerStorage const& storage) = 0;
};

} // namespace digitalcurling::players
