// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerNormalDist

#pragma once

#include <memory>
#include <optional>
#include <random>
#include <string>
#include "digitalcurling/players/i_player.hpp"

#include "player_normal_dist_factory.hpp"
#include "player_normal_dist_storage.hpp"

namespace digitalcurling::players {

class PlayerNormalDistStorage;

/// @brief Player that caps the initial shot speed, then adds normally distributed random variation to the initial speed and angle
class PlayerNormalDist : public IPlayer {
public:
    /// @brief Constructor
    /// @param factory Factory for this player
    PlayerNormalDist(PlayerNormalDistFactory const& factory);
    /// @brief Constructor
    /// @param storage Storage for this player
    PlayerNormalDist(PlayerNormalDistStorage const& storage);
    virtual ~PlayerNormalDist() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }

    virtual Gender GetGender() const override { return factory_.gender; }
    virtual std::unique_ptr<IPlayerFactory> GetFactory() const override { return std::make_unique<PlayerNormalDistFactory>(factory_); }

    virtual moves::Shot Play(moves::Shot const& shot) override;

    virtual std::unique_ptr<IPlayerStorage> CreateStorage() const override;
    virtual void Save(IPlayerStorage & storage) const override;
    virtual void Load(IPlayerStorage const& storage) override;

private:
    PlayerNormalDistFactory factory_;
    std::optional<std::mt19937> engine_;
    std::normal_distribution<float> speed_dist_;
    std::normal_distribution<float> angle_dist_;

    void LoadEngine(
        std::string const& engine_data,
        std::string const& speed_dist_data,
        std::string const& angle_dist_data
    );
};

} // namespace digitalcurling::players
