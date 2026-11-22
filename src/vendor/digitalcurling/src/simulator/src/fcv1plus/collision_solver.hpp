// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <vector>
#include "fixed_types.hpp"

namespace digitalcurling::simulators::fcv1plus_detail::collision_solver {

// A sequential pass can propagate an impulse across at least one additional
// contact in a reverse-indexed chain. kStoneMax passes therefore cover every
// edge of the longest possible chain (which has kStoneMax - 1 edges).
inline constexpr int kCollisionResolutionPasses = StoneCoordinate::kStoneMax;

// Mode B is the shipped behavior: applies friction torque using the ideal disc
// lever radius, updating translational and angular velocity consistently.
inline constexpr bool kApplyFrictionTorque = true;

struct CollisionResult {
    std::uint8_t a;
    std::uint8_t b;
    WideFixed normal_velocity_impulse;
    WideFixed tangent_velocity_impulse;

    CollisionResult(std::uint8_t a_value, std::uint8_t b_value) noexcept
        : a(a_value)
        , b(b_value)
        , normal_velocity_impulse(0)
        , tangent_velocity_impulse(0)
    {}
};

using StartPositions = std::array<FixedVector2, StoneCoordinate::kStoneMax>;

std::vector<CollisionResult> Resolve(
    AllStonesFixed& stones,
    StartPositions const* start_positions = nullptr);

// Exposes explicit Mode A/B selection to unit tests. The production Resolve()
// path above uses compile-time-selected Mode B (kApplyFrictionTorque = true).
std::vector<CollisionResult> ResolveForTesting(
    AllStonesFixed& stones,
    bool apply_friction_torque,
    int* passes_used = nullptr,
    StartPositions const* start_positions = nullptr);

} // namespace digitalcurling::simulators::fcv1plus_detail::collision_solver
