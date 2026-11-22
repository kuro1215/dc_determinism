// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerND2Factory

#pragma once

#include <random>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_factory.hpp"

#include "nd2.hpp"

namespace digitalcurling::players {


/// @brief Parameters of player ND2
struct PlayerND2Parameter {
    /// @brief Maximum shot speed
    /// Must be finite and greater than 0.
    float max_speed = 4.0f;
    /// @brief Standard deviation of the normally distributed variation added to the initial shot speed
    /// Must be finite and at least 0. 0 adds no variation.
    float stddev_speed = 0;
    /// @brief Standard deviation of the normally distributed variation added to the initial shot angle
    /// Must be finite and at least 0. 0 adds no variation.
    float stddev_angle = 0;
    /// @brief Standard deviation of the normally distributed variation added to the shot rotation
    /// Must be finite and at least 0. 0 adds no variation.
    float stddev_rotation = 0;
};

/// @brief Factory of player ND2
class PlayerND2Factory : public IPlayerFactory {
public:
    /// @brief Player gender
    Gender gender = Gender::kUnknown;

    /// @brief Parameters for clockwise shots
    PlayerND2Parameter cw = {};

    /// @brief Parameters for counterclockwise shots
    PlayerND2Parameter ccw = {};

    /// @brief Random seed
    /// If `std::nullopt`, the seed is set randomly.
    std::optional<std::random_device::result_type> seed = std::nullopt;

    /// @brief Default constructor
    PlayerND2Factory() = default;
    /// @brief Copy constructor
    PlayerND2Factory(PlayerND2Factory const&) = default;
    /// @brief Copy assignment operator
    PlayerND2Factory & operator = (PlayerND2Factory const&) = default;
    virtual ~PlayerND2Factory() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;
    virtual std::unique_ptr<IPlayerFactory> Clone() const override;

    /// @brief Checks the parameters of both rotation directions
    /// @throws std::invalid_argument If `max_speed` is not finite and greater than 0,
    ///         or a standard deviation is not finite and at least 0
    void Validate() const;
};


/// @cond Doxygen_Suppress
// json
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlayerND2Parameter, max_speed, stddev_speed, stddev_angle, stddev_rotation)

void to_json(nlohmann::json &, PlayerND2Factory const&);
void from_json(nlohmann::json const&, PlayerND2Factory &);
/// @endcond

} // namespace digitalcurling::players
