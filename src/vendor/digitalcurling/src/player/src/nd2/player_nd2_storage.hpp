// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerND2Storage

#pragma once

#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_storage.hpp"

#include "player_nd2_factory.hpp"

namespace digitalcurling::players {


/// @brief Storage of player ND2
class PlayerND2Storage : public IPlayerStorage {
public:
    /// @brief Default constructor
    PlayerND2Storage() = default;
    /// @brief Copy constructor
    PlayerND2Storage(PlayerND2Storage const&) = default;
    /// @brief Copy assignment operator
    PlayerND2Storage & operator = (PlayerND2Storage const&) = default;
    virtual ~PlayerND2Storage() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return factory.gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;

    /// @brief Factory information for the player saved in this storage
    PlayerND2Factory factory;
    /// @brief Engine state data
    /// A default-constructed storage holds the state of an engine seeded with 0.
    std::string engine_data = DefaultEngineData();

private:
    static std::string DefaultEngineData();
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, PlayerND2Storage const&);
void from_json(nlohmann::json const&, PlayerND2Storage &);
/// @endcond

} // namespace digitalcurling::players
