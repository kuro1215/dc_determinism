// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "collision_solver.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include "digitalcurling/stone.hpp"

namespace digitalcurling::simulators::fcv1plus_detail::collision_solver {
namespace {

Fixed const kRadius(Stone::kRadius);
Fixed const kDiameter(Stone::kRadius * 2.0f);
Fixed const kFriction(0.2f);
Fixed const kTwo(2);
Fixed const kSix(6);

struct PairAccumulator {
    bool had_impulse;
    WideFixed normal;
    WideFixed tangent;

    PairAccumulator() noexcept
        : had_impulse(false)
        , normal(0)
        , tangent(0)
    {}
};

constexpr std::size_t kPairCount =
    static_cast<std::size_t>(StoneCoordinate::kStoneMax) *
    (static_cast<std::size_t>(StoneCoordinate::kStoneMax) - 1) / 2;

constexpr std::size_t PairIndex(std::size_t i, std::size_t j)
{
    // Precondition: i < j < StoneCoordinate::kStoneMax.
    auto const n = static_cast<std::size_t>(StoneCoordinate::kStoneMax);
    return i * (2 * n - 1 - i) / 2 + (j - i - 1);
}

constexpr bool PairIndexIsBijection()
{
    std::array<bool, kPairCount> seen{};
    auto const n = static_cast<std::size_t>(StoneCoordinate::kStoneMax);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            auto const index = PairIndex(i, j);
            if (index >= kPairCount || seen[index]) {
                return false;
            }
            seen[index] = true;
        }
    }
    for (bool const value : seen) {
        if (!value) {
            return false;
        }
    }
    return true;
}

static_assert(
    PairIndexIsBijection(),
    "PairIndex must be a bijection onto [0, kPairCount)");

using Accumulators = std::array<PairAccumulator, kPairCount>;

Fixed FromRawDifference(std::int64_t raw)
{
    if (raw < std::numeric_limits<std::int32_t>::min() ||
        raw > std::numeric_limits<std::int32_t>::max())
    {
        throw std::overflow_error("fcv1plus: relative position exceeds Q8.24 range.");
    }
    return Fixed::from_raw_value(static_cast<std::int32_t>(raw));
}

FixedVector2 RelativePosition(StoneStateFixed const& a, StoneStateFixed const& b)
{
    return {
        FromRawDifference(static_cast<std::int64_t>(b.position.x.raw_value()) -
                          static_cast<std::int64_t>(a.position.x.raw_value())),
        FromRawDifference(static_cast<std::int64_t>(b.position.y.raw_value()) -
                          static_cast<std::int64_t>(a.position.y.raw_value()))
    };
}

FixedVector2 RelativeVelocity(StoneStateFixed const& a, StoneStateFixed const& b)
{
    return {
        FromRawDifference(static_cast<std::int64_t>(b.velocity.x.raw_value()) -
                          static_cast<std::int64_t>(a.velocity.x.raw_value())),
        FromRawDifference(static_cast<std::int64_t>(b.velocity.y.raw_value()) -
                          static_cast<std::int64_t>(a.velocity.y.raw_value()))
    };
}

Fixed RadiusTimesAngularSum(Fixed lhs, Fixed rhs)
{
    auto const sum_raw =
        static_cast<std::int64_t>(lhs.raw_value()) +
        static_cast<std::int64_t>(rhs.raw_value());
    auto product = static_cast<std::int64_t>(kRadius.raw_value()) * sum_raw;
    auto const half = std::int64_t{1} << 23;
    product += product >= 0 ? half : -half;
    auto const result_raw = product / (std::int64_t{1} << 24);
    if (result_raw < std::numeric_limits<std::int32_t>::min() ||
        result_raw > std::numeric_limits<std::int32_t>::max())
    {
        throw std::overflow_error(
            "fcv1plus: angular contact velocity exceeds Q8.24 range.");
    }
    return Fixed::from_raw_value(static_cast<std::int32_t>(result_raw));
}

Fixed NarrowWideFixed(WideFixed value, char const* description)
{
    // Q16.16 raw values in [-128, 128) are exactly safe to scale to Q8.24.
    constexpr std::int64_t kFixedMagnitudeInWideRaw =
        std::int64_t{128} << 16;
    auto const raw = static_cast<std::int64_t>(value.raw_value());
    if (raw < -kFixedMagnitudeInWideRaw || raw >= kFixedMagnitudeInWideRaw) {
        throw std::overflow_error(
            std::string("fcv1plus: ") + description +
            " exceeds Q8.24 range.");
    }
    return Fixed(value);
}

Fixed FrictionAngularDelta(Fixed tangent_delta_speed)
{
    auto const angular_delta =
        WideFixed(kTwo) * WideFixed(tangent_delta_speed) / WideFixed(kRadius);
    return NarrowWideFixed(angular_delta, "friction torque angular delta");
}

void AddVelocity(StoneStateFixed& stone, FixedVector2 const& delta)
{
    stone.velocity.x = CheckedAdd(
        stone.velocity.x, delta.x, Fixed(kRuntimeSpeed), "velocity.x");
    stone.velocity.y = CheckedAdd(
        stone.velocity.y, delta.y, Fixed(kRuntimeSpeed), "velocity.y");
}

void AddPosition(StoneStateFixed& stone, FixedVector2 const& delta)
{
    stone.position.x = CheckedAdd(
        stone.position.x, delta.x, Fixed(kRuntimeLimit), "position.x");
    stone.position.y = CheckedAdd(
        stone.position.y, delta.y, Fixed(kRuntimeLimit), "position.y");
}

template <bool ApplyFrictionTorque>
bool ResolvePair(
    StoneStateFixed& a,
    StoneStateFixed& b,
    FixedVector2 const& normal,
    Fixed distance,
    PairAccumulator& accumulated)
{
    bool changed = false;
    auto const tangent = normal.Skew();
    auto const relative_velocity = RelativeVelocity(a, b);
    auto const relative_normal_velocity = Dot(relative_velocity, normal);

    if (relative_normal_velocity < Fixed(0)) {
        changed = true;
        // Verified sign convention from the approved design:
        // vA += vrel_n*n, vB -= vrel_n*n, where vrel_n is negative.
        auto const normal_delta = normal * relative_normal_velocity;
        AddVelocity(a, normal_delta);
        AddVelocity(b, -normal_delta);
        accumulated.normal += WideFixed(-relative_normal_velocity);

        Fixed relative_tangent_velocity(0);
        Fixed divisor(2);
        if constexpr (ApplyFrictionTorque) {
            relative_tangent_velocity =
                Dot(relative_velocity, tangent) -
                RadiusTimesAngularSum(
                    a.angular_velocity,
                    b.angular_velocity);
            divisor = kSix;
        } else {
            relative_tangent_velocity = Dot(relative_velocity, tangent);
        }

        auto const friction_limit = kFriction * fpm::abs(relative_normal_velocity);
        // delta_t is explicitly "the velocity change applied to B".
        auto const tangent_delta_speed = std::clamp(
            -relative_tangent_velocity / divisor,
            -friction_limit,
            friction_limit);
        auto const tangent_delta = tangent * tangent_delta_speed;
        AddVelocity(b, tangent_delta);
        AddVelocity(a, -tangent_delta);

        if constexpr (ApplyFrictionTorque) {
            // Verified Mode B signs from the approved design. Both disks receive
            // -2*delta_t/r because their contact radii have opposite signs.
            auto const angular_delta = FrictionAngularDelta(tangent_delta_speed);
            a.angular_velocity = CheckedAdd(
                a.angular_velocity,
                -angular_delta,
                Fixed(kRuntimeAngularVelocity),
                "angular_velocity");
            b.angular_velocity = CheckedAdd(
                b.angular_velocity,
                -angular_delta,
                Fixed(kRuntimeAngularVelocity),
                "angular_velocity");
        }

        accumulated.tangent += WideFixed(tangent_delta_speed);
        accumulated.had_impulse = true;
    }

    auto const penetration = kDiameter - distance;
    if (penetration > Fixed(0)) {
        auto const correction = normal * (penetration / kTwo);
        changed = changed ||
            correction.x.raw_value() != 0 ||
            correction.y.raw_value() != 0;
        AddPosition(a, -correction);
        AddPosition(b, correction);
    }
    return changed;
}

struct ContactCandidate {
    std::uint64_t distance_squared;
    std::uint8_t a;
    std::uint8_t b;
    std::uint32_t entry_key;
};

constexpr std::uint32_t kEntryKeyScale = std::uint32_t{1} << 24;

bool RawPairOverlaps(
    std::int64_t dx_raw,
    std::int64_t dy_raw,
    std::int64_t diameter_raw,
    std::uint64_t diameter_squared) noexcept
{
    auto const abs_dx = static_cast<std::uint64_t>(
        dx_raw < 0 ? -dx_raw : dx_raw);
    auto const abs_dy = static_cast<std::uint64_t>(
        dy_raw < 0 ? -dy_raw : dy_raw);
    if (abs_dx > static_cast<std::uint64_t>(diameter_raw) ||
        abs_dy > static_cast<std::uint64_t>(diameter_raw))
    {
        return false;
    }
    return abs_dx * abs_dx + abs_dy * abs_dy <= diameter_squared;
}

std::int64_t InterpolateRaw(
    std::int64_t start_raw,
    std::int64_t end_raw,
    std::uint32_t entry_key) noexcept
{
    // Runtime positions are bounded to +/-64 m, so a relative component can
    // move by at most 256 m in one frame. In Q8.24, |delta| <= 2^32;
    // multiplying by this Q0.24 key is at most 2^56 and fits in int64_t.
    auto const delta = end_raw - start_raw;
    return start_raw +
        delta * static_cast<std::int64_t>(entry_key) /
            static_cast<std::int64_t>(kEntryKeyScale);
}

bool PairOverlapsAt(
    FixedVector2 const& start_a,
    FixedVector2 const& end_a,
    FixedVector2 const& start_b,
    FixedVector2 const& end_b,
    std::uint32_t entry_key,
    std::int64_t diameter_raw,
    std::uint64_t diameter_squared) noexcept
{
    // Interpolate the relative vector once. Interpolating the two centers
    // separately would introduce two independent truncations, which can move
    // an exactly tangential comparison by one raw unit.
    auto const start_x =
        static_cast<std::int64_t>(start_b.x.raw_value()) -
        static_cast<std::int64_t>(start_a.x.raw_value());
    auto const start_y =
        static_cast<std::int64_t>(start_b.y.raw_value()) -
        static_cast<std::int64_t>(start_a.y.raw_value());
    auto const end_x =
        static_cast<std::int64_t>(end_b.x.raw_value()) -
        static_cast<std::int64_t>(end_a.x.raw_value());
    auto const end_y =
        static_cast<std::int64_t>(end_b.y.raw_value()) -
        static_cast<std::int64_t>(end_a.y.raw_value());
    return RawPairOverlaps(
        InterpolateRaw(start_x, end_x, entry_key),
        InterpolateRaw(start_y, end_y, entry_key),
        diameter_raw, diameter_squared);
}

std::uint32_t EstimateEntryKey(
    FixedVector2 const& start_a,
    FixedVector2 const& end_a,
    FixedVector2 const& start_b,
    FixedVector2 const& end_b,
    std::int64_t diameter_raw,
    std::uint64_t diameter_squared) noexcept
{
    if (PairOverlapsAt(
            start_a, end_a, start_b, end_b, 0,
            diameter_raw, diameter_squared))
    {
        return 0;
    }

    // The caller has established overlap at kEntryKeyScale. A line segment
    // enters a disk at most once before an overlapping endpoint, so this
    // fixed 24-step binary search finds the first Q0.24 key whose interpolated
    // centers overlap. It uses only integer arithmetic and no heap storage.
    std::uint32_t outside = 0;
    std::uint32_t inside = kEntryKeyScale;
    while (inside - outside > 1) {
        auto const middle = outside + (inside - outside) / 2;
        if (PairOverlapsAt(
                start_a, end_a, start_b, end_b, middle,
                diameter_raw, diameter_squared))
        {
            inside = middle;
        } else {
            outside = middle;
        }
    }
    return inside;
}

template <bool ApplyFrictionTorque>
std::vector<CollisionResult> ResolveImpl(
    AllStonesFixed& stones,
    int* passes_used,
    StartPositions const* start_positions)
{
    Accumulators accumulated;
    std::array<std::uint8_t, StoneCoordinate::kStoneMax> enabled_indices{};
    std::size_t enabled_count = 0;
    for (std::size_t index = 0; index < stones.size(); ++index) {
        if (stones[index].enabled) {
            enabled_indices[enabled_count++] =
                static_cast<std::uint8_t>(index);
        }
    }
    auto const diameter_raw = static_cast<std::int64_t>(kDiameter.raw_value());
    auto const diameter_squared =
        static_cast<std::uint64_t>(diameter_raw * diameter_raw);

    // Pass 0 builds at most 120 (16 choose 2) fixed-size end-overlap contact
    // candidates, sorted by estimated chronological entry time within the frame.
    // If entry keys are equal (simultaneous contacts), ties break deterministically
    // by ascending pair index (a, then b). Candidates are revalidated before
    // resolution to reflect mutations caused by earlier pairs.
    // Subsequent passes (pass >= 1) retain the standard ascending pair iteration
    // for chain propagation.
    for (int pass = 0; pass < kCollisionResolutionPasses; ++pass) {
        if (passes_used != nullptr) {
            *passes_used = pass + 1;
        }
        bool pass_changed = false;

        if (pass == 0) {
            std::array<ContactCandidate, kPairCount> candidates;
            std::size_t candidate_count = 0;

            for (std::size_t i_position = 0;
                 i_position < enabled_count;
                 ++i_position)
            {
                auto const i =
                    static_cast<std::size_t>(enabled_indices[i_position]);
                for (std::size_t j_position = i_position + 1;
                     j_position < enabled_count;
                     ++j_position)
                {
                    auto const j =
                        static_cast<std::size_t>(enabled_indices[j_position]);
                    auto const dx_raw =
                        static_cast<std::int64_t>(stones[j].position.x.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.x.raw_value());
                    auto const dy_raw =
                        static_cast<std::int64_t>(stones[j].position.y.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.y.raw_value());

                    auto const abs_dx = static_cast<std::uint64_t>(
                        dx_raw < 0 ? -dx_raw : dx_raw);
                    auto const abs_dy = static_cast<std::uint64_t>(
                        dy_raw < 0 ? -dy_raw : dy_raw);
                    if (abs_dx > static_cast<std::uint64_t>(diameter_raw) ||
                        abs_dy > static_cast<std::uint64_t>(diameter_raw))
                    {
                        continue;
                    }

                    auto const distance_squared =
                        abs_dx * abs_dx + abs_dy * abs_dy;
                    if (distance_squared > diameter_squared) {
                        continue;
                    }

                    candidates[candidate_count++] = {
                        distance_squared,
                        static_cast<std::uint8_t>(i),
                        static_cast<std::uint8_t>(j),
                        0
                    };
                }
            }

            // A lone contact has no ordering decision, so do not estimate an
            // entry key for it. Candidate collection is already (a, b) order.
            if (start_positions != nullptr && candidate_count > 1) {
                for (std::size_t c = 0; c < candidate_count; ++c) {
                    auto const i = static_cast<std::size_t>(candidates[c].a);
                    auto const j = static_cast<std::size_t>(candidates[c].b);
                    candidates[c].entry_key = EstimateEntryKey(
                        (*start_positions)[i], stones[i].position,
                        (*start_positions)[j], stones[j].position,
                        diameter_raw, diameter_squared);
                }

                // Deterministic sort: ascending entry time; ties by (a, b).
                std::sort(
                    candidates.begin(),
                    candidates.begin() + candidate_count,
                    [](ContactCandidate const& lhs, ContactCandidate const& rhs) {
                        if (lhs.entry_key != rhs.entry_key) {
                            return lhs.entry_key < rhs.entry_key;
                        }
                        if (lhs.a != rhs.a) {
                            return lhs.a < rhs.a;
                        }
                        return lhs.b < rhs.b;
                    });
            }

            for (std::size_t c = 0; c < candidate_count; ++c) {
                auto const i = static_cast<std::size_t>(candidates[c].a);
                auto const j = static_cast<std::size_t>(candidates[c].b);
                auto distance_squared = candidates[c].distance_squared;

                // The first candidate follows collection without a mutation,
                // so its recorded end-overlap is still valid. Later candidates
                // must be revalidated after earlier pair corrections.
                if (c != 0) {
                    auto const dx_raw =
                        static_cast<std::int64_t>(stones[j].position.x.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.x.raw_value());
                    auto const dy_raw =
                        static_cast<std::int64_t>(stones[j].position.y.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.y.raw_value());

                    auto const abs_dx = static_cast<std::uint64_t>(
                        dx_raw < 0 ? -dx_raw : dx_raw);
                    auto const abs_dy = static_cast<std::uint64_t>(
                        dy_raw < 0 ? -dy_raw : dy_raw);
                    if (abs_dx > static_cast<std::uint64_t>(diameter_raw) ||
                        abs_dy > static_cast<std::uint64_t>(diameter_raw))
                    {
                        continue;
                    }

                    distance_squared = abs_dx * abs_dx + abs_dy * abs_dy;
                    if (distance_squared > diameter_squared) {
                        continue;
                    }
                }

                auto const delta = RelativePosition(stones[i], stones[j]);
                FixedVector2 normal;
                Fixed distance(0);
                if (distance_squared == 0) {
                    auto const relative_velocity =
                        RelativeVelocity(stones[i], stones[j]);
                    if (relative_velocity.SquaredRawLength() == 0) {
                        normal = {Fixed(1), Fixed(0)};
                    } else {
                        // Approved degenerate-case sign: opposite relative
                        // velocity so dot(vB-vA,n) is necessarily negative.
                        normal = -relative_velocity.Normalize();
                    }
                } else {
                    distance = delta.Length();
                    normal = delta / distance;
                }

                bool const pair_changed = ResolvePair<ApplyFrictionTorque>(
                    stones[i],
                    stones[j],
                    normal,
                    distance,
                    accumulated[PairIndex(i, j)]);
                pass_changed = pass_changed || pair_changed;
                ValidateRuntimeState(stones[i]);
                ValidateRuntimeState(stones[j]);
            }
        } else {
            // Passes >= 1: retain bounded follow-up passes in ascending pair order for chain propagation
            for (std::size_t i_position = 0;
                 i_position < enabled_count;
                 ++i_position)
            {
                auto const i =
                    static_cast<std::size_t>(enabled_indices[i_position]);
                for (std::size_t j_position = i_position + 1;
                     j_position < enabled_count;
                     ++j_position)
                {
                    auto const j =
                        static_cast<std::size_t>(enabled_indices[j_position]);
                    auto const dx_raw =
                        static_cast<std::int64_t>(stones[j].position.x.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.x.raw_value());
                    auto const dy_raw =
                        static_cast<std::int64_t>(stones[j].position.y.raw_value()) -
                        static_cast<std::int64_t>(stones[i].position.y.raw_value());

                    auto const abs_dx = static_cast<std::uint64_t>(
                        dx_raw < 0 ? -dx_raw : dx_raw);
                    auto const abs_dy = static_cast<std::uint64_t>(
                        dy_raw < 0 ? -dy_raw : dy_raw);
                    if (abs_dx > static_cast<std::uint64_t>(diameter_raw) ||
                        abs_dy > static_cast<std::uint64_t>(diameter_raw))
                    {
                        continue;
                    }

                    auto const distance_squared =
                        abs_dx * abs_dx + abs_dy * abs_dy;
                    if (distance_squared > diameter_squared) {
                        continue;
                    }

                    auto const delta = RelativePosition(stones[i], stones[j]);
                    FixedVector2 normal;
                    Fixed distance(0);
                    if (distance_squared == 0) {
                        auto const relative_velocity =
                            RelativeVelocity(stones[i], stones[j]);
                        if (relative_velocity.SquaredRawLength() == 0) {
                            normal = {Fixed(1), Fixed(0)};
                        } else {
                            normal = -relative_velocity.Normalize();
                        }
                    } else {
                        distance = delta.Length();
                        normal = delta / distance;
                    }

                    bool const pair_changed = ResolvePair<ApplyFrictionTorque>(
                        stones[i],
                        stones[j],
                        normal,
                        distance,
                        accumulated[PairIndex(i, j)]);
                    pass_changed = pass_changed || pair_changed;
                    ValidateRuntimeState(stones[i]);
                    ValidateRuntimeState(stones[j]);
                }
            }
        }

        if (!pass_changed) {
            break;
        }
    }

    std::vector<CollisionResult> results;
    for (std::size_t i_position = 0;
         i_position < enabled_count;
         ++i_position)
    {
        auto const i =
            static_cast<std::size_t>(enabled_indices[i_position]);
        for (std::size_t j_position = i_position + 1;
             j_position < enabled_count;
             ++j_position)
        {
            auto const j =
                static_cast<std::size_t>(enabled_indices[j_position]);
            auto const& value = accumulated[PairIndex(i, j)];
            if (!value.had_impulse) {
                continue;
            }
            CollisionResult result(
                static_cast<std::uint8_t>(i),
                static_cast<std::uint8_t>(j));
            result.normal_velocity_impulse = value.normal;
            result.tangent_velocity_impulse = value.tangent;
            results.emplace_back(result);
        }
    }
    return results;
}

} // namespace

std::vector<CollisionResult> Resolve(
    AllStonesFixed& stones,
    StartPositions const* start_positions)
{
    if constexpr (kApplyFrictionTorque) {
        return ResolveImpl<true>(stones, nullptr, start_positions);
    } else {
        return ResolveImpl<false>(stones, nullptr, start_positions);
    }
}

std::vector<CollisionResult> ResolveForTesting(
    AllStonesFixed& stones,
    bool apply_friction_torque,
    int* passes_used,
    StartPositions const* start_positions)
{
    return apply_friction_torque
        ? ResolveImpl<true>(stones, passes_used, start_positions)
        : ResolveImpl<false>(stones, passes_used, start_positions);
}

} // namespace digitalcurling::simulators::fcv1plus_detail::collision_solver
