// Copyright (c) 2022-2025 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerIdenticalFactory

#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_factory.hpp"

#include "identical.hpp"

namespace digitalcurling::players {


/// @brief Factory for the Identical player
class PlayerIdenticalFactory : public IPlayerFactory {
public:
    /// @brief Player gender
    Gender gender = Gender::kUnknown;

    /// @brief Default constructor
    PlayerIdenticalFactory() = default;
    /// @brief Copy constructor
    PlayerIdenticalFactory(PlayerIdenticalFactory const&) = default;
    /// @brief Copy assignment operator
    PlayerIdenticalFactory & operator = (PlayerIdenticalFactory const&) = default;
    virtual ~PlayerIdenticalFactory() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;
    virtual std::unique_ptr<IPlayerFactory> Clone() const override;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, PlayerIdenticalFactory const&);
void from_json(nlohmann::json const&, PlayerIdenticalFactory &);
/// @endcond

} // namespace digitalcurling::players
