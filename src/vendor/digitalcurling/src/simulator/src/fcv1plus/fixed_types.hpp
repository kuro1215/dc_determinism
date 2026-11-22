// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <fpm/fixed.hpp>
#include <fpm/math.hpp>
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/stone.hpp"

namespace digitalcurling::simulators::fcv1plus_detail {

using Fixed = fpm::fixed_8_24;
using WideFixed = fpm::fixed_16_16;

inline constexpr float kSupportedPositionX = 10.0f;
inline constexpr float kSupportedPositionY = 50.0f;
inline constexpr float kSupportedSpeed = 10.0f;
inline constexpr float kSupportedAngularVelocity = 20.0f;
inline constexpr float kSupportedAngle = 100.0f;

// Runtime limits are deliberately below Q8.24's approximately +/-128 range.
// 64 is a power of two (2^30 in the underlying Q8.24 representation), so a
// plain magnitude comparison against it is already about as cheap as possible.
// The limits leave room for collision intermediates while covering the complete
// sheet (y = 43.892 m) and realistic curling velocities.
inline constexpr float kRuntimeLimit = 64.0f;
inline constexpr float kRuntimeSpeed = 48.0f;
inline constexpr float kRuntimeAngularVelocity = 96.0f;

inline constexpr float kSheetOutPositionX =
    coordinate::kSheetWidthMax / 2.0f;
inline constexpr float kSheetOutPositionY = coordinate::kBackBoardY;

inline std::uint64_t AbsRaw(std::int32_t value) noexcept
{
    auto const wide = static_cast<std::int64_t>(value);
    return static_cast<std::uint64_t>(wide < 0 ? -wide : wide);
}

inline std::uint64_t IntegerSquareRoot(std::uint64_t value) noexcept
{
    // Restoring binary square root. All operations are unsigned and have a
    // platform-independent order; no intermediate can exceed uint64_t.
    std::uint64_t result = 0;
    std::uint64_t bit = std::uint64_t{1} << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    // Round to the nearest raw value. At this point value is n - result^2;
    // the midpoint to (result + 1)^2 is result + 0.5.
    if (value > result) {
        ++result;
    }
    return result;
}

struct FixedVector2 {
    Fixed x;
    Fixed y;

    FixedVector2() noexcept : x(0), y(0) {}
    FixedVector2(Fixed x_value, Fixed y_value) noexcept : x(x_value), y(y_value) {}

    std::uint64_t SquaredRawLength() const noexcept
    {
        auto const ax = AbsRaw(x.raw_value());
        auto const ay = AbsRaw(y.raw_value());
        return ax * ax + ay * ay;
    }

    Fixed Length() const
    {
        auto const raw_length = IntegerSquareRoot(SquaredRawLength());
        if (raw_length > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
            throw std::overflow_error("fcv1plus: vector length exceeds Q8.24 range.");
        }
        return Fixed::from_raw_value(static_cast<std::int32_t>(raw_length));
    }

    FixedVector2 Normalize() const
    {
        auto const length = Length();
        if (length == Fixed(0)) {
            throw std::domain_error("fcv1plus: cannot normalize a zero vector.");
        }
        return {x / length, y / length};
    }

    FixedVector2 Skew() const noexcept { return {-y, x}; }

    FixedVector2& operator+=(FixedVector2 const& value) noexcept
    {
        x += value.x;
        y += value.y;
        return *this;
    }
    FixedVector2& operator-=(FixedVector2 const& value) noexcept
    {
        x -= value.x;
        y -= value.y;
        return *this;
    }
};

inline FixedVector2 operator+(FixedVector2 lhs, FixedVector2 const& rhs) noexcept
{
    return lhs += rhs;
}
inline FixedVector2 operator-(FixedVector2 lhs, FixedVector2 const& rhs) noexcept
{
    return lhs -= rhs;
}
inline FixedVector2 operator-(FixedVector2 value) noexcept
{
    return {-value.x, -value.y};
}
inline FixedVector2 operator*(FixedVector2 const& value, Fixed scalar) noexcept
{
    return {value.x * scalar, value.y * scalar};
}
inline FixedVector2 operator*(Fixed scalar, FixedVector2 const& value) noexcept
{
    return value * scalar;
}
inline FixedVector2 operator/(FixedVector2 const& value, Fixed scalar) noexcept
{
    return {value.x / scalar, value.y / scalar};
}
inline Fixed Dot(FixedVector2 const& lhs, FixedVector2 const& rhs) noexcept
{
    return lhs.x * rhs.x + lhs.y * rhs.y;
}

inline Fixed CheckedFixedFromFloat(float value, float absolute_limit, char const* field)
{
    if (!std::isfinite(value) || std::abs(value) > absolute_limit) {
        throw std::invalid_argument(std::string("fcv1plus: invalid ") + field + ".");
    }
    return Fixed(value);
}

inline FixedVector2 ToFixedPosition(Vector2 const& value)
{
    return {
        CheckedFixedFromFloat(value.x, kSupportedPositionX, "position.x"),
        CheckedFixedFromFloat(value.y, kSupportedPositionY, "position.y")
    };
}

inline FixedVector2 ValidateStonePositionForSetStones(Vector2 const& value)
{
    constexpr float kPositionXLimit =
        coordinate::kSheetWidthMax / 2.0f - Stone::kRadius;
    constexpr float kPositionYLimit =
        coordinate::kBackBoardY - Stone::kRadius;
    if (!std::isfinite(value.x) ||
        value.x <= -kPositionXLimit ||
        value.x >= kPositionXLimit)
    {
        throw std::invalid_argument("fcv1plus: invalid position.x.");
    }
    if (!std::isfinite(value.y) ||
        value.y < 0.0f ||
        value.y >= kPositionYLimit)
    {
        throw std::invalid_argument("fcv1plus: invalid position.y.");
    }
    return ToFixedPosition(value);
}

inline FixedVector2 ToFixedVelocity(Vector2 const& value)
{
    if (!std::isfinite(value.x) || !std::isfinite(value.y) ||
        std::hypot(value.x, value.y) > kSupportedSpeed)
    {
        throw std::invalid_argument("fcv1plus: invalid translational velocity.");
    }
    return {Fixed(value.x), Fixed(value.y)};
}

inline Vector2 ToDigitalCurlingVector2(FixedVector2 const& value) noexcept
{
    return {static_cast<float>(value.x), static_cast<float>(value.y)};
}

struct StoneStateFixed {
    bool enabled;
    FixedVector2 position;
    Fixed angle;
    FixedVector2 velocity;
    Fixed angular_velocity;

    StoneStateFixed() noexcept
        : enabled(false)
        , position()
        , angle(0)
        , velocity()
        , angular_velocity(0)
    {}
};

using AllStonesFixed = std::array<StoneStateFixed, StoneCoordinate::kStoneMax>;

inline Fixed CheckedAdd(Fixed lhs, Fixed rhs, Fixed absolute_limit, char const* field)
{
    auto const raw = static_cast<std::int64_t>(lhs.raw_value()) +
                     static_cast<std::int64_t>(rhs.raw_value());
    auto const limit = static_cast<std::int64_t>(absolute_limit.raw_value());
    if (raw < -limit || raw > limit ||
        raw < std::numeric_limits<std::int32_t>::min() ||
        raw > std::numeric_limits<std::int32_t>::max())
    {
        throw std::overflow_error(std::string("fcv1plus: runtime ") + field + " limit exceeded.");
    }
    return Fixed::from_raw_value(static_cast<std::int32_t>(raw));
}

inline void ValidateRuntimeState(StoneStateFixed const& stone)
{
    if (!stone.enabled) {
        return;
    }
    // Widen position values to int64_t, and use AbsRaw for the remaining
    // magnitude checks, instead of fpm::abs()/unary operator-. Those helpers
    // negate the raw int32_t value directly and are undefined behavior when
    // it is exactly INT32_MIN. This function runs on state restored from
    // potentially corrupted or adversarial Load()/JSON input, so that raw
    // value is a reachable input, not just a theoretical one.
    auto const position_x_raw =
        static_cast<std::int64_t>(stone.position.x.raw_value());
    auto const position_y_raw =
        static_cast<std::int64_t>(stone.position.y.raw_value());
    auto const position_limit =
        static_cast<std::int64_t>(Fixed(kRuntimeLimit).raw_value());
    auto const angular_velocity_limit =
        static_cast<std::uint64_t>(Fixed(kRuntimeAngularVelocity).raw_value());

    // Position checked first, exactly as before: short-circuits before
    // velocity is examined at all, matching the original || chain's order.
    if (position_x_raw < -position_limit ||
        position_x_raw > position_limit ||
        position_y_raw < -position_limit ||
        position_y_raw > position_limit)
    {
        throw std::overflow_error("fcv1plus: state left the supported runtime range.");
    }

    // Inlined, sqrt-free equivalent of the old `velocity.Length()` call and
    // its own internal overflow throw -- same exception, same message, same
    // position in the overall precedence (only reached once position is
    // known valid, exactly as the original short-circuiting order required).
    auto const n = stone.velocity.SquaredRawLength();
    constexpr std::uint64_t kMaxRawLength =
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
    if (n > kMaxRawLength * kMaxRawLength + kMaxRawLength) {
        throw std::overflow_error("fcv1plus: vector length exceeds Q8.24 range.");
    }
    auto const speed_limit_raw =
        static_cast<std::uint64_t>(Fixed(kRuntimeSpeed).raw_value());
    bool const speed_exceeded =
        n > speed_limit_raw * speed_limit_raw + speed_limit_raw;

    if (speed_exceeded ||
        AbsRaw(stone.angular_velocity.raw_value()) > angular_velocity_limit)
    {
        throw std::overflow_error("fcv1plus: state left the supported runtime range.");
    }
}

} // namespace digitalcurling::simulators::fcv1plus_detail
