// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Shot

#pragma once

#include <cmath>
#include <string_view>
#include <nlohmann/json.hpp>
#include "digitalcurling/vector2.hpp"

namespace digitalcurling::moves {


/// @brief Move: shot
/// @sa Move
struct Shot {
    /// @brief Type name identifying the `Move`
    static constexpr std::string_view kTypeName = "shot";

    /// @brief Shot translational velocity
    float translational_velocity;
    /// @brief Shot angular velocity
    float angular_velocity;
    /// @brief Shot angle (rad)
    float release_angle;

    /// @brief Default constructor
    Shot() : translational_velocity(0.f), angular_velocity(0.f), release_angle(0.f) {}

    /// @brief Constructor
    Shot(float translational_velocity, float angular_velocity, float release_angle)
        : translational_velocity(translational_velocity),
          angular_velocity(angular_velocity),
          release_angle(release_angle) {}

    /// @brief Gets the shot's velocity vector
    /// @returns Vector
    Vector2 ToVector2() const {
        return Vector2(
            translational_velocity * std::cos(release_angle),
            translational_velocity * std::sin(release_angle)
        );
    }
};


/// @cond Doxygen_Suppress
// json
inline void to_json(nlohmann::json & j, Shot const& v) {
    j["translational_velocity"] = v.translational_velocity;
    j["angular_velocity"] = v.angular_velocity;
    j["release_angle"] = v.release_angle;
}
inline void from_json(nlohmann::json const& j, Shot & v) {
    v.translational_velocity = digitalcurling::json_util::GetCheckedFloatAt(j, "translational_velocity");
    v.angular_velocity = digitalcurling::json_util::GetCheckedFloatAt(j, "angular_velocity");
    v.release_angle = digitalcurling::json_util::GetCheckedFloatAt(j, "release_angle");
}
/// @endcond

} // namespace digitalcurling::moves
