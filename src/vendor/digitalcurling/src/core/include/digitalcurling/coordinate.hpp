// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines coordinate constants

#pragma once

#include <cmath>
#include "digitalcurling/vector2.hpp"

namespace digitalcurling::coordinate {

/// @brief Y coordinate of the hog line
inline constexpr float kHogLineY = 32.004f;

/// @brief Y coordinate of the tee line
inline constexpr float kTeeLineY = 38.405f;

/// @brief Y coordinate of the back line
inline constexpr float kBackLineY = 40.234f;

/// @brief Y coordinate of the hack
inline constexpr float kHackY = 42.063f;

/// @brief Y coordinate of the backboard
inline constexpr float kBackBoardY = 43.892f;

/// @brief Radius of the house
inline constexpr float kHouseRadius = 1.829f;

/// @brief Minimum sheet width
inline constexpr float kSheetWidthMin = 4.420f;

/// @brief Maximum sheet width
inline constexpr float kSheetWidthMax = 4.750f;

/// @brief Coordinates of the center of the house
inline constexpr Vector2 kTee = Vector2(0.f, coordinate::kTeeLineY);

inline bool IsValidSheetWidth(float width) noexcept
{
    return std::isfinite(width) && width >= kSheetWidthMin && width <= kSheetWidthMax;
}

} // namespace digitalcurling::coordinate
