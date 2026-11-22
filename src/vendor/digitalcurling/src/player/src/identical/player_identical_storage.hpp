// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerIdenticalStorage

#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_storage.hpp"

#include "player_identical_factory.hpp"

namespace digitalcurling::players {


/// @brief Storage for the Identical player
class PlayerIdenticalStorage : public IPlayerStorage {
public:
    /// @brief Player gender
    Gender gender = Gender::kUnknown;

    /// @brief Default constructor
    PlayerIdenticalStorage() = default;
    /// @brief Copy constructor
    PlayerIdenticalStorage(PlayerIdenticalStorage const&) = default;
    /// @brief Copy assignment operator
    PlayerIdenticalStorage& operator = (PlayerIdenticalStorage const&) = default;
    virtual ~PlayerIdenticalStorage() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, PlayerIdenticalStorage const&);
void from_json(nlohmann::json const&, PlayerIdenticalStorage &);
/// @endcond

} // namespace digitalcurling::players
