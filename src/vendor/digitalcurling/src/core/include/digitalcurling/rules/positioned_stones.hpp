// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines PositionedStonesPattern

#pragma once

#include <stdexcept>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/stone.hpp"
#include "digitalcurling/vector2.hpp"

namespace digitalcurling {

namespace rules {

/// @brief Pre-positioned stone pattern in mixed doubles
enum class PositionedStonesPattern {
    kPosition1Front = 3,
    kPosition1Back = 2,
    kPosition2Front = 1,
    kPosition2Back = 0,
    kPosition3Front = 5,
    kPosition3Back = 4,
    kPosition4Back = 6,
    kPosition4Front = 7
};

/// @brief Pre-positioned stone type in mixed doubles
enum class PositionedStonesType {
    kCenterGuard,
    kPowerPlayLeft,
    kPowerPlayRight
};


/// @cond Doxygen_Suppress
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(PositionedStonesPattern, {
    { PositionedStonesPattern::kPosition1Front, "position1_front" },
    { PositionedStonesPattern::kPosition1Back, "position1_back" },
    { PositionedStonesPattern::kPosition2Front, "position2_front" },
    { PositionedStonesPattern::kPosition2Back, "position2_back" },
    { PositionedStonesPattern::kPosition3Front, "position3_front" },
    { PositionedStonesPattern::kPosition3Back, "position3_back" },
    { PositionedStonesPattern::kPosition4Front, "position4_front" },
    { PositionedStonesPattern::kPosition4Back, "position4_back" },
});
/// @endcond

} // namespace digitalcurling::rules

namespace coordinate {

inline Vector2 GetHammerTeamPositionedStonePoint(rules::PositionedStonesType type = rules::PositionedStonesType::kCenterGuard) {
    constexpr float feet_to_meters = 1.0f / 3.28084f;

    switch (type) {
        case rules::PositionedStonesType::kCenterGuard:
            return Vector2(0.f, kTeeLineY + 2.0f * feet_to_meters - Stone::kRadius);
        case rules::PositionedStonesType::kPowerPlayLeft:
            return Vector2(-4.0f * feet_to_meters, kTeeLineY - Stone::kRadius);
        case rules::PositionedStonesType::kPowerPlayRight:
            return Vector2(4.0f * feet_to_meters, kTeeLineY - Stone::kRadius);
        default:
            throw std::invalid_argument("Invalid PositionedStonesType value");
    }
};

inline Vector2 GetNonHammerTeamPositionedStonePoint(
    rules::PositionedStonesPattern pattern,
    rules::PositionedStonesType type = rules::PositionedStonesType::kCenterGuard
) {
    float x = 0.f, y = 0.f;
    bool is_power_play = type == rules::PositionedStonesType::kPowerPlayLeft ||
        type == rules::PositionedStonesType::kPowerPlayRight;

    constexpr float position1_y = 35.204f;
    constexpr float position2_y = 34.595f;
    constexpr float position3_y = 33.985f;
    constexpr float position4_y = 33.376f;

    switch (pattern) {
        case rules::PositionedStonesPattern::kPosition1Front:
            y = position1_y - Stone::kRadius;
            if (is_power_play) x = 1.12f;
            break;
        case rules::PositionedStonesPattern::kPosition1Back:
            y = position1_y + Stone::kRadius;
            if (is_power_play) x = 1.12f;
            break;
        case rules::PositionedStonesPattern::kPosition2Front:
            y = position2_y - Stone::kRadius;
            if (is_power_play) x = 1.10f;
            break;
        case rules::PositionedStonesPattern::kPosition2Back:
            y = position2_y + Stone::kRadius;
            if (is_power_play) x = 1.10f;
            break;
        case rules::PositionedStonesPattern::kPosition3Front:
            y = position3_y - Stone::kRadius;
            if (is_power_play) x = 1.08f;
            break;
        case rules::PositionedStonesPattern::kPosition3Back:
            y = position3_y + Stone::kRadius;
            if (is_power_play) x = 1.08f;
            break;
        case rules::PositionedStonesPattern::kPosition4Front:
            y = position4_y - Stone::kRadius;
            if (is_power_play) x = 1.07f;
            break;
        case rules::PositionedStonesPattern::kPosition4Back:
            y = position4_y + Stone::kRadius;
            if (is_power_play) x = 1.07f;
            break;
        default:
            throw std::invalid_argument("Invalid PositionedStonesPattern value");
    }

    switch (type) {
        case rules::PositionedStonesType::kCenterGuard:
            return Vector2(0.f, y);
        case rules::PositionedStonesType::kPowerPlayLeft:
            return Vector2(-x, y);
        case rules::PositionedStonesType::kPowerPlayRight:
            return Vector2(x, y);
        default:
            throw std::invalid_argument("Invalid PositionedStonesType value");
    }
};

} // namespace digitalcurling::coordinate

} // namespace digitalcurling
