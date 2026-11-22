// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <fpm/math.hpp>
#include "fixed_types.hpp"

namespace digitalcurling::simulators::fcv1plus_detail {

// Use a deterministic cubic approximation only for the small sliding yaw.
// Larger angles retain the previous fpm::sin behavior.
inline Fixed SineForSlide(Fixed angle) noexcept
{
    auto const raw = angle.raw_value();
    auto const magnitude = AbsRaw(raw);
    constexpr std::uint64_t kMaxRaw = std::uint64_t{1} << 20; // 1/16 rad
    if (magnitude > kMaxRaw) {
        return fpm::sin(angle);
    }

    // The raw correction is |raw|^3 / (6 * 2^48). At this limit the cube
    // is at most 2^60, and adding half the denominator still fits uint64_t.
    // Round the correction to nearest, with exact ties away from zero;
    // restoring the sign afterwards keeps the approximation odd.
    constexpr std::uint64_t kDenominator = 6 * (std::uint64_t{1} << 48);
    auto const cube = magnitude * magnitude * magnitude;
    auto const correction = (cube + kDenominator / 2) / kDenominator;
    auto const result = static_cast<std::int32_t>(magnitude - correction);
    return Fixed::from_raw_value(raw < 0 ? -result : result);
}

} // namespace digitalcurling::simulators::fcv1plus_detail
