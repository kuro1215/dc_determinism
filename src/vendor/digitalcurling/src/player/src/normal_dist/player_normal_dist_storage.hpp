// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerNormalDistStorage

#pragma once

#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_storage.hpp"

#include "player_normal_dist_factory.hpp"

namespace digitalcurling::players {


/// @brief Normal distribution data for the NormalDist player
struct NormalDistData {
};

/// @brief Storage for the NormalDist player
class PlayerNormalDistStorage : public IPlayerStorage {
public:
    /// @brief Default constructor
    PlayerNormalDistStorage() = default;
    /// @brief Copy constructor
    PlayerNormalDistStorage(PlayerNormalDistStorage const&) = default;
    /// @brief Copy assignment operator
    PlayerNormalDistStorage & operator = (PlayerNormalDistStorage const&) = default;
    virtual ~PlayerNormalDistStorage() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return factory.gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;

    /// @brief Factory information for the player saved in this storage
    PlayerNormalDistFactory factory;
    /// @brief Engine state data
    std::string engine_data;
    /// @brief Speed distribution data
    std::string speed_dist_data;
    /// @brief Angle distribution data
    std::string angle_dist_data;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, PlayerNormalDistStorage const&);
void from_json(nlohmann::json const&, PlayerNormalDistStorage &);
/// @endcond

} // namespace digitalcurling::players
