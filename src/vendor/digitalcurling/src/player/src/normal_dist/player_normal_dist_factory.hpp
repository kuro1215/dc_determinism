// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PlayerNormalDistFactory

#pragma once

#include <random>
#include <memory>
#include <optional>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player_factory.hpp"

#include "normal_dist.hpp"

namespace digitalcurling::players {


/// @brief Factory for the NormalDist player
class PlayerNormalDistFactory : public IPlayerFactory {
public:
    /// @brief Player gender
    Gender gender = Gender::kUnknown;

    /// @brief Maximum shot speed
    /// Must be finite and greater than 0.
    float max_speed = 4.f;

    /// @brief Standard deviation of the normally distributed variation added to the initial shot speed
    /// Must be finite and greater than 0 (precondition of `std::normal_distribution`).
    float stddev_speed = 0.0076f;

    /// @brief Standard deviation of the normally distributed variation added to the initial shot angle
    /// Must be finite and greater than 0 (precondition of `std::normal_distribution`).
    float stddev_angle = 0.0018f;

    /// @brief Random seed
    /// If `std::nullopt`, the seed is set randomly.
    std::optional<std::random_device::result_type> seed = std::nullopt;

    /// @brief Default constructor
    PlayerNormalDistFactory() = default;
    /// @brief Copy constructor
    PlayerNormalDistFactory(PlayerNormalDistFactory const&) = default;
    /// @brief Copy assignment operator
    PlayerNormalDistFactory & operator = (PlayerNormalDistFactory const&) = default;
    virtual ~PlayerNormalDistFactory() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual Gender GetGender() const override { return gender; }
    virtual std::unique_ptr<IPlayer> CreatePlayer() const override;
    virtual std::unique_ptr<IPlayerFactory> Clone() const override;

    /// @brief Checks the parameters
    /// @throws std::invalid_argument If `max_speed` or a standard deviation is not finite and greater than 0
    void Validate() const;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, PlayerNormalDistFactory const&);
void from_json(nlohmann::json const&, PlayerNormalDistFactory &);
/// @endcond

} // namespace digitalcurling::players
