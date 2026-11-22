// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <box2d/box2d.h>
#include "digitalcurling/vector2.hpp"

namespace digitalcurling::simulators {


/// @brief Converts to a Box2D b2Vec2
/// @param v Vector (digitalcurling::Vector2)
/// @return Vector (b2Vec2)
inline b2Vec2 ToB2Vec2(Vector2 v)
{
    return { v.x, v.y };
}
/// @brief Converts to a DigitalCurling Vector2
/// @param v Vector (b2Vec2)
/// @return Vector (digitalcurling::Vector2)
inline Vector2 ToDigitalCurlingVector2(b2Vec2 v)
{
    return { v.x, v.y };
}

} // namespace digitalcurling::simulators
