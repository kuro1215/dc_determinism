// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerND2

#pragma once

#include <memory>
#include <optional>
#include <random>
#include <string>
#include "digitalcurling/players/i_player.hpp"

#include "player_nd2_factory.hpp"
#include "player_nd2_storage.hpp"
#include "xoshiro256plusplus.hpp"

namespace digitalcurling::players {


/// @brief Player that caps the initial shot speed, then adds normally distributed random variation to the initial speed and angle
class PlayerND2 : public IPlayer {
public:
    /// @brief Constructor
    /// @param factory Factory for this player
    PlayerND2(PlayerND2Factory const& factory);
    /// @brief Constructor
    /// @param storage Storage for this player
    PlayerND2(PlayerND2Storage const& storage);
    virtual ~PlayerND2() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }

    virtual Gender GetGender() const override { return factory_.gender; }
    virtual std::unique_ptr<IPlayerFactory> GetFactory() const override { return std::make_unique<PlayerND2Factory>(factory_); }

    virtual moves::Shot Play(moves::Shot const& shot) override;

    virtual std::unique_ptr<IPlayerStorage> CreateStorage() const override;
    virtual void Save(IPlayerStorage & storage) const override;
    virtual void Load(IPlayerStorage const& storage) override;

private:
    PlayerND2Factory factory_;
    std::optional<Xoshiro256PlusPlus> engine_;

    void LoadEngine(std::string const& engine_data);
    float GenerateND2ribution(float sigma);
};

} // namespace digitalcurling::players
