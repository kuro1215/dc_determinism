// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "simulator_fcv1plus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <fpm/math.hpp>
#include "collision_solver.hpp"
#include "slide_sine.hpp"

namespace digitalcurling::simulators {
namespace {

using namespace fcv1plus_detail;

Fixed WrapAngle(Fixed angle) noexcept
{
    auto const pi = static_cast<std::int64_t>(Fixed::pi().raw_value());
    auto const two_pi = static_cast<std::int64_t>(Fixed::two_pi().raw_value());
    auto raw = static_cast<std::int64_t>(angle.raw_value());
    raw = (raw + pi) % two_pi;
    if (raw < 0) {
        raw += two_pi;
    }
    raw -= pi;
    return Fixed::from_raw_value(static_cast<std::int32_t>(raw));
}

StoneStateFixed FromRawStone(SimulatorFCV1PlusRawStone const& raw)
{
    StoneStateFixed result;
    result.enabled = raw.enabled;
    result.position = {
        Fixed::from_raw_value(raw.position_x),
        Fixed::from_raw_value(raw.position_y)};
    result.angle = Fixed::from_raw_value(raw.angle);
    result.velocity = {
        Fixed::from_raw_value(raw.velocity_x),
        Fixed::from_raw_value(raw.velocity_y)};
    result.angular_velocity = Fixed::from_raw_value(raw.angular_velocity);
    ValidateRuntimeState(result);
    return result;
}

SimulatorFCV1PlusRawStone ToRawStone(StoneStateFixed const& stone) noexcept
{
    if (!stone.enabled) {
        return {};
    }
    SimulatorFCV1PlusRawStone result;
    result.enabled = true;
    result.position_x = stone.position.x.raw_value();
    result.position_y = stone.position.y.raw_value();
    result.angle = stone.angle.raw_value();
    result.velocity_x = stone.velocity.x.raw_value();
    result.velocity_y = stone.velocity.y.raw_value();
    result.angular_velocity = stone.angular_velocity.raw_value();
    return result;
}

ISimulator::StoneState ToPublicStone(StoneStateFixed const& stone) noexcept
{
    return {
        ToDigitalCurlingVector2(stone.position),
        static_cast<float>(stone.angle),
        ToDigitalCurlingVector2(stone.velocity),
        static_cast<float>(stone.angular_velocity)
    };
}

AllStonesFixed RestoreStonesFromStorage(
    SimulatorFCV1PlusStorage const& storage)
{
    AllStonesFixed restored;
    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        restored[i] = FromRawStone(storage.stones[i]);
        if (restored[i].enabled &&
            (restored[i].angle < -Fixed::pi() ||
             restored[i].angle >= Fixed::pi()))
        {
            throw std::invalid_argument(
                "SimulatorFCV1Plus::Load: angle is not normalized.");
        }
    }
    return restored;
}

} // namespace

SimulatorFCV1Plus::SimulatorFCV1Plus(SimulatorFCV1PlusFactory const& factory)
    : SimulatorFCV1Plus(SimulatorFCV1PlusStorage(factory))
{}

SimulatorFCV1Plus::SimulatorFCV1Plus(SimulatorFCV1PlusStorage const& storage)
    : storage_(storage)
    , stones_fixed_()
    , stones_cache_()
    , stones_dirty_(true)
    , all_stones_stopped_(true)
    , all_stones_stopped_dirty_(true)
{
    stones_fixed_ = RestoreStonesFromStorage(storage_);
    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

void SimulatorFCV1Plus::SetStoneInner(
    int index,
    std::optional<StoneState> const& stone)
{
    if (!stone.has_value()) {
        stones_fixed_[index] = StoneStateFixed();
        return;
    }

    StoneStateFixed converted;
    converted.enabled = true;
    converted.position = ValidateStonePositionForSetStones(stone->position);
    converted.angle = WrapAngle(CheckedFixedFromFloat(
        stone->angle, kSupportedAngle, "angle"));
    converted.velocity = ToFixedVelocity(stone->translational_velocity);
    converted.angular_velocity = CheckedFixedFromFloat(
        stone->angular_velocity,
        kSupportedAngularVelocity,
        "angular_velocity");
    ValidateRuntimeState(converted);
    stones_fixed_[index] = converted;
}

void SimulatorFCV1Plus::SetStones(AllStones const& stones)
{
    // Convert into a temporary first, giving the boundary operation a strong
    // exception guarantee if any one input is invalid.
    AllStonesFixed converted;
    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        if (!stones[i].has_value()) {
            continue;
        }
        converted[i].enabled = true;
        converted[i].position =
            ValidateStonePositionForSetStones(stones[i]->position);
        converted[i].angle = WrapAngle(CheckedFixedFromFloat(
            stones[i]->angle, kSupportedAngle, "angle"));
        converted[i].velocity = ToFixedVelocity(
            stones[i]->translational_velocity);
        converted[i].angular_velocity = CheckedFixedFromFloat(
            stones[i]->angular_velocity,
            kSupportedAngularVelocity,
            "angular_velocity");
        ValidateRuntimeState(converted[i]);
    }
    stones_fixed_ = converted;
    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

void SimulatorFCV1Plus::SetStone(
    StoneIndex index,
    std::optional<StoneState> const& stone)
{
    if ((index.GetTeam() != Team::k0 && index.GetTeam() != Team::k1) ||
        index.GetStoneNumber() >= 8)
    {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::SetStone: index is out of range.");
    }
    SetStoneInner(index.GetIndex(), stone);
    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

void SimulatorFCV1Plus::Step()
{
    using namespace fcv1plus_detail;
    auto const dt = Fixed(kSecondsPerFrame);
    auto const epsilon = std::numeric_limits<Fixed>::epsilon();
    auto const zero = Fixed(0);

    std::array<FixedVector2, StoneCoordinate::kStoneMax> start_positions{};
    for (std::size_t i = 0; i < stones_fixed_.size(); ++i) {
        auto& stone = stones_fixed_[i];
        if (!stone.enabled) {
            continue;
        }
        start_positions[i] = stone.position;
        if (stone.velocity.x == zero && stone.velocity.y == zero &&
            stone.angular_velocity == zero)
        {
            continue;
        }

        auto const stone_speed = stone.velocity.Length();
        auto const angular_velocity = stone.angular_velocity;

        // These constants and formula structure intentionally match fcv1.
        constexpr float kGravity = 9.80665f;
        if (stone_speed > epsilon) {
            auto const longitudinal_acceleration =
                -(Fixed(0.00200985f) /
                      (stone_speed + Fixed(0.06385782f)) +
                  Fixed(0.00626286f)) *
                Fixed(kGravity);
            auto const new_stone_speed =
                stone_speed + longitudinal_acceleration * dt;
            if (new_stone_speed <= zero) {
                stone.velocity = FixedVector2();
            } else {
                Fixed yaw_rate(0);
                if (fpm::abs(angular_velocity) > epsilon) {
                    // pow is deliberately evaluated in Q16.16. At dt=0.001
                    // the smallest speed reaching this branch keeps pow well
                    // inside its +/-32768 range; no behavior-changing clamp.
                    auto const speed_wide = WideFixed(stone_speed);
                    auto const yaw_rate_wide =
                        WideFixed(0.00820f) *
                        fpm::pow(speed_wide, WideFixed(-0.8f));
                    yaw_rate = Fixed(yaw_rate_wide);
                    if (angular_velocity < zero) {
                        yaw_rate = -yaw_rate;
                    }
                }

                auto const yaw = yaw_rate * dt;
                auto const longitudinal_velocity =
                    new_stone_speed * fpm::cos(yaw);
                auto const transverse_velocity =
                    new_stone_speed * SineForSlide(yaw);
                auto const longitudinal_direction = stone.velocity / stone_speed;
                auto const transverse_direction =
                    longitudinal_direction.Skew();
                stone.velocity =
                    longitudinal_direction * longitudinal_velocity +
                    transverse_direction * transverse_velocity;
            }
        } else {
            // Keep this branch's zero-velocity outcome consistent with
            // AreAllStonesStopped(), which treats any nonzero raw velocity
            // as "still moving". Without this, a stone whose speed sits in
            // (0, epsilon] would never be touched here (the damping branch
            // above requires a strict >) while AreAllStonesStopped() would
            // report it as perpetually not-stopped, hanging any
            // `while (!AreAllStonesStopped()) Step();` caller.
            stone.velocity = FixedVector2();
        }

        if (fpm::abs(angular_velocity) > epsilon) {
            auto const angular_accel =
                -Fixed(0.025f) /
                std::max(stone_speed, Fixed(0.001f)) *
                dt;
            if (fpm::abs(angular_velocity) <= fpm::abs(angular_accel)) {
                stone.angular_velocity = zero;
            } else {
                // Apply the signed decrement directly: multiplying two small
                // fixed-point values first can round away the damping.
                stone.angular_velocity = angular_velocity > zero
                    ? angular_velocity + angular_accel
                    : angular_velocity - angular_accel;
            }
        }

        // Symplectic Euler: integrate with the newly damped velocities.
        stone.position.x = CheckedAdd(
            stone.position.x,
            stone.velocity.x * dt,
            Fixed(kRuntimeLimit),
            "position.x");
        stone.position.y = CheckedAdd(
            stone.position.y,
            stone.velocity.y * dt,
            Fixed(kRuntimeLimit),
            "position.y");
        stone.angle = WrapAngle(
            CheckedAdd(
                stone.angle,
                stone.angular_velocity * dt,
                Fixed(kSupportedAngle),
                "angle"));
        ValidateRuntimeState(stone);
    }

    // The solver updates stones_fixed_; its collision reports are only used by the solver tests.
    static_cast<void>(collision_solver::Resolve(stones_fixed_, &start_positions));

    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;

    // This internal safety net deliberately uses the wider raw sheet edges.
    // The shared IsValidOnSimulation() check runs after Step() and uses the
    // narrower, radius-subtracted playable region; the two bounds serve
    // different purposes.
    for (auto& stone : stones_fixed_) {
        if (!stone.enabled) {
            continue;
        }
        if (AbsRaw(stone.position.x.raw_value()) >
                static_cast<std::uint64_t>(
                    Fixed(kSheetOutPositionX).raw_value()) ||
            stone.position.y < Fixed(0) ||
            stone.position.y > Fixed(kSheetOutPositionY))
        {
            stone.enabled = false;
        }
    }
}

ISimulator::AllStones SimulatorFCV1Plus::GetStones() const
{
    if (stones_dirty_) {
        for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
            if (stones_fixed_[i].enabled) {
                stones_cache_[i] = ToPublicStone(stones_fixed_[i]);
            } else {
                stones_cache_[i] = std::nullopt;
            }
        }
        stones_dirty_ = false;
    }
    return stones_cache_;
}

bool SimulatorFCV1Plus::AreAllStonesStopped() const
{
    if (all_stones_stopped_dirty_) {
        all_stones_stopped_ = true;
        auto const epsilon = std::numeric_limits<Fixed>::epsilon();
        for (auto const& stone : stones_fixed_) {
            if (!stone.enabled) {
                continue;
            }
            if (stone.velocity.SquaredRawLength() != 0 ||
                fpm::abs(stone.angular_velocity) > epsilon)
            {
                all_stones_stopped_ = false;
                break;
            }
        }
        all_stones_stopped_dirty_ = false;
    }
    return all_stones_stopped_;
}

float SimulatorFCV1Plus::GetSecondsPerFrame() const
{
    return kSecondsPerFrame;
}

std::unique_ptr<ISimulatorFactory> SimulatorFCV1Plus::GetFactory() const
{
    return std::make_unique<SimulatorFCV1PlusFactory>(storage_.factory);
}

void SimulatorFCV1Plus::UpdateStorageRawStones() const
{
    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        storage_.stones[i] = ToRawStone(stones_fixed_[i]);
    }
}

std::unique_ptr<ISimulatorStorage> SimulatorFCV1Plus::CreateStorage() const
{
    UpdateStorageRawStones();
    return std::make_unique<SimulatorFCV1PlusStorage>(storage_);
}

void SimulatorFCV1Plus::Save(SimulatorFCV1PlusStorage& storage) const
{
    UpdateStorageRawStones();
    storage = storage_;
}

void SimulatorFCV1Plus::Load(SimulatorFCV1PlusStorage const& storage)
{
    // Validate and restore into a temporary before changing either member.
    auto const restored = RestoreStonesFromStorage(storage);
    storage_ = storage;
    stones_fixed_ = restored;
    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

moves::Shot SimulatorFCV1Plus::CalculateShot(
    Vector2 const& target_position,
    float target_speed,
    float shot_angular_velocity) const
{
    using namespace fcv1plus_detail;
    if (!std::isfinite(target_speed) || target_speed < 0.0f) {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::CalculateShot: target_speed must be finite and non-negative.");
    }
    if (target_speed > 4.0f) {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::CalculateShot: target_speed is too large.");
    }
    if (!std::isfinite(shot_angular_velocity) ||
        shot_angular_velocity == 0.0f ||
        std::abs(shot_angular_velocity) > kSupportedAngularVelocity)
    {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::CalculateShot: invalid shot angular velocity.");
    }

    auto const target_fixed = ToFixedPosition(target_position);
    auto const target_r = target_fixed.Length();
    auto const target_speed_fixed = Fixed(target_speed);

    Fixed domain_limit(0);
    if (target_speed_fixed <= Fixed(0.05f)) {
        domain_limit = Fixed(29.898958358378636);
    } else if (target_speed_fixed <= Fixed(1.0f)) {
        domain_limit = Fixed(29.86751291726946);
    } else {
        domain_limit = Fixed(8.228225657195706);
    }
    // Compare after quantization, before fpm::log can observe a non-positive
    // argument. Equality is invalid because the domain is strictly greater.
    if (target_r <= domain_limit) {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::CalculateShot: target_position is outside the regression domain.");
    }

    auto const v0_speed = [&] {
        Fixed c0(0);
        Fixed c1(0);
        Fixed c2(0);
        if (target_speed_fixed <= Fixed(0.05f)) {
            c0 = Fixed(0.0005048122574925176) * target_r +
                 Fixed(0.2756242531609261);
            c1 = -Fixed(0.00046669575066030805) *
                     fpm::log(target_r + Fixed(-29.898958358378636)) +
                 Fixed(-0.0014030973174948508);
            c2 = Fixed(0.13968687866736632) * target_r +
                 Fixed(0.41120940058777616);
        } else if (target_speed_fixed <= Fixed(1.0f)) {
            c0 = Fixed(-0.0014309170115803444) * target_r +
                 Fixed(0.9858457898438147);
            c1 = -Fixed(-0.0008339331735471273) *
                     fpm::log(target_r + Fixed(-29.86751291726946)) +
                 Fixed(-0.19811799977982522);
            c2 = Fixed(0.13967323742978) * target_r +
                 Fixed(0.42816312110477517);
        } else {
            // Horner evaluation is required: target_r^3 would overflow Q8.24.
            c0 = Fixed(1.0833113118071224e-06);
            c0 = c0 * target_r + Fixed(-0.00012132851917870833);
            c0 = c0 * target_r + Fixed(0.004578093297561233);
            c0 = c0 * target_r + Fixed(0.9767006869364527);
            c1 = -Fixed(0.07950648211492622) *
                     fpm::log(target_r + Fixed(-8.228225657195706)) +
                 Fixed(-0.05601306077702578);
            c2 = Fixed(0.14140440186382008) * target_r +
                 Fixed(0.3875782508767419);
        }
        auto const radicand =
            c0 * target_speed_fixed * target_speed_fixed +
            c1 * target_speed_fixed +
            c2;
        if (radicand <= Fixed(0)) {
            throw std::domain_error(
                "SimulatorFCV1Plus::CalculateShot: invalid regression radicand.");
        }
        return fpm::sqrt(radicand);
    }();

    if (target_speed_fixed >= v0_speed) {
        throw std::invalid_argument(
            "SimulatorFCV1Plus::CalculateShot: target_speed is too large for the target_position.");
    }

    auto const angular_velocity =
        shot_angular_velocity > 0.0f ? Fixed::half_pi() : -Fixed::half_pi();
    auto const delta = [&] {
        // Do not cache across factories: every call uses this instance's full
        // factory configuration, avoiding fcv1's thread-local keying bug.
        auto simulator_base = GetFactory()->CreateSimulator();
        auto& simulator = dynamic_cast<SimulatorFCV1Plus&>(*simulator_base);
        AllStones initial;
        initial[0].emplace(
            Vector2(),
            0.0f,
            Vector2(0.0f, static_cast<float>(v0_speed)),
            static_cast<float>(angular_velocity));
        simulator.SetStones(initial);

        while (!simulator.AreAllStonesStopped()) {
            auto const speed = simulator.stones_fixed_[0].velocity.Length();
            if (speed <= target_speed_fixed) {
                return simulator.stones_fixed_[0].position;
            }
            simulator.Step();
        }
        return simulator.stones_fixed_[0].position;
    }();

    auto const delta_angle = fpm::atan2(delta.x, delta.y);
    auto const target_angle = fpm::atan2(target_fixed.y, target_fixed.x);
    auto const release_angle = target_angle + delta_angle;
    return {
        static_cast<float>(v0_speed),
        static_cast<float>(angular_velocity),
        static_cast<float>(release_angle)
    };
}

SimulatorFCV1PlusStorage::RawStones
SimulatorFCV1Plus::GetRawStonesForTesting() const
{
    SimulatorFCV1PlusStorage::RawStones result;
    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        result[i] = ToRawStone(stones_fixed_[i]);
    }
    return result;
}

} // namespace digitalcurling::simulators
