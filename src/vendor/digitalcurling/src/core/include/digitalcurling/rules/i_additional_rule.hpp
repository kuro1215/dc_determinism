// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IAdditionalRule

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/stone_coordinate.hpp"

namespace digitalcurling::rules {


/// @brief Type of additional rule
enum class AdditionalRuleTypes {
    /// @brief Free guard zone
    kFreeGuardZone,
    /// @brief No-tick shot
    kNoTickShot,
};

/// @brief Interface for an additional rule
class IAdditionalRule {
public:
    /// @brief Constructor
    IAdditionalRule() {}
    virtual ~IAdditionalRule() = default;

    /// @brief Gets the rule type
    /// @return Rule type
    virtual AdditionalRuleTypes GetType() const = 0;
};

/// @brief Interface for additional rules applied to shots
class IShotRule : public IAdditionalRule {
public:
    /// @brief Number of shots to which the rule applies
    std::uint8_t applied_shot_count;

    /// @brief Constructor
    /// @param[in] applied_shot_count Number of shots to which the rule applies
    IShotRule(std::uint8_t applied_shot_count) : applied_shot_count(applied_shot_count), IAdditionalRule() { }
    virtual ~IShotRule() = default;

    /// @brief Checks whether the shot follows the rule
    /// @param[in] shot_in_end Shot number within the end
    /// @param[in] deliver Team that delivered the shot
    /// @param[in] before Board state before the shot
    /// @param[in] after Board state after the shot
    /// @return `true` if the shot follows the rule, otherwise `false`
    virtual bool ValidateShot(std::uint8_t shot_in_end, Team deliver, StoneCoordinate before, StoneCoordinate after) const = 0;
};


/// @cond Doxygen_Suppress
// json
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(AdditionalRuleTypes, {
    { AdditionalRuleTypes::kFreeGuardZone, "free_guard_zone" },
    { AdditionalRuleTypes::kNoTickShot, "no_tick_shot" },
});
/// @endcond

} // namespace digitalcurling::rules
