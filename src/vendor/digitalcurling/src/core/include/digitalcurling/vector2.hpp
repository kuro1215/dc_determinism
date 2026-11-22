// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Vector2

#pragma once

#include <cmath>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

namespace digitalcurling {


/// @brief Two-dimensional vector
struct Vector2 {
    /// @brief X coordinate
    float x;
    /// @brief Y coordinate
    float y;

    /// @brief Initializes to (0, 0)
    constexpr Vector2() : x(0.f), y(0.f) {}

    /// @brief Initializes with the specified coordinates
    /// @param[in] x X coordinate
    /// @param[in] y Y coordinate
    constexpr Vector2(float x, float y) : x(x), y(y) {}

    /// @brief Gets the squared length of the vector
    /// @returns Squared length of the vector (`x*x + y*y`)
    constexpr float SquaredLength() const
    {
        return x * x + y * y;
    }

    /// @brief Gets the length of the vector
    /// @returns Length of the vector (`std::hypot(x, y)`)
    float Length() const
    {
        return std::hypot(x, y);
    }


    /// @cond Doxygen_Suppress
    // equation operators
    friend constexpr bool operator == (Vector2 v1, Vector2 v2) {
        return v1.x == v2.x && v1.y == v2.y;
    }
    friend constexpr bool operator != (Vector2 v1, Vector2 v2) {
        return !(v1 == v2);
    }

    // arithmetic operators
    constexpr Vector2 & operator += (Vector2 v) {
        x += v.x;
        y += v.y;
        return *this;
    }
    constexpr Vector2 & operator -= (Vector2 v) {
        x -= v.x;
        y -= v.y;
        return *this;
    }
    constexpr Vector2 & operator *= (float f) {
        x *= f;
        y *= f;
        return *this;
    }
    constexpr Vector2 & operator /= (float f) {
        x /= f;
        y /= f;
        return *this;
    }

    constexpr Vector2 operator-(Vector2 v) {
        return { -v.x, -v.y };
    }
    friend constexpr Vector2 operator+(Vector2 v1, Vector2 v2) {
        return { v1.x + v2.x, v1.y + v2.y };
    }
    friend constexpr Vector2 operator-(Vector2 v1, Vector2 v2) {
        return { v1.x - v2.x, v1.y - v2.y };
    }

    friend constexpr Vector2 operator*(float f, Vector2 v) {
        return { f * v.x, f * v.y };
    }
    friend constexpr Vector2 operator*(Vector2 v, float f) {
        return f * v;
    }
    friend constexpr Vector2 operator/(Vector2 v, float f) {
        return { v.x / f, v.y / f };
    }
    /// @endcond
};


/// @cond Doxygen_Suppress
// json
inline void to_json(nlohmann::json & j, Vector2 const& v) {
    j["x"] = v.x;
    j["y"] = v.y;
}
inline void from_json(nlohmann::json const& j, Vector2 & v) {
    v.x = json_util::GetCheckedFloatAt(j, "x");
    v.y = json_util::GetCheckedFloatAt(j, "y");
}
/// @endcond

} // namespace digitalcurling
