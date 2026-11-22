// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Stone

#pragma once

#include <nlohmann/json.hpp>
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/vector2.hpp"

namespace digitalcurling {


/// @brief Position and angle of the stone
struct Stone {

    /// @brief Stone radius (m)
    static constexpr float kRadius = 0.145f;

    /// @brief Stone position
    Vector2 position;
    /// @brief Stone angle
    float angle;

    /// @brief Initializes position to (0, 0) and angle to zero
    constexpr Stone() : position(), angle(0.f) {}

    /// @brief Initializes from the specified values
    /// @param[in] position Position
    /// @param[in] angle Angle
    constexpr Stone(Vector2 position, float angle) : position(position), angle(angle) {}

    /// @brief Gets the distance from the center of the stone to the center of the house
    /// @return Distance between the house and the stone
    float GetDistanceFromTee() const {
        return (position - coordinate::kTee).Length();
    }

    /// @brief Checks whether the stone is in the house
    /// @return `true` if the stone is in the house
    bool IsInHouse() const {
        return GetDistanceFromTee() < coordinate::kHouseRadius + kRadius;
    }
};


/// @cond Doxygen_Suppress
// json
inline void to_json(nlohmann::json & j, Stone const& v) {
    j["position"] = v.position;
    j["angle"] = v.angle;
}
inline void from_json(nlohmann::json const& j, Stone & v) {
    j.at("position").get_to(v.position);
    v.angle = json_util::GetCheckedFloatAt(j, "angle");
}
/// @endcond

} // namespace digitalcurling
