// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include "simulator_fcv1.hpp"

namespace digitalcurling::simulators {

namespace {

// Rejects a stone state with a non-finite field. No magnitude limits are applied.
void ValidateStoneState(std::optional<ISimulator::StoneState> const& stone)
{
    if (!stone.has_value()) return;

    if (!std::isfinite(stone->position.x) || !std::isfinite(stone->position.y) ||
        !std::isfinite(stone->angle) ||
        !std::isfinite(stone->translational_velocity.x) || !std::isfinite(stone->translational_velocity.y) ||
        !std::isfinite(stone->angular_velocity))
    {
        throw std::invalid_argument("SimulatorFCV1: stone state must not contain non-finite values.");
    }
}

void ValidateStones(ISimulator::AllStones const& stones)
{
    for (auto const& stone : stones) {
        ValidateStoneState(stone);
    }
}

} // unnamed namespace

SimulatorFCV1::SimulatorFCV1(SimulatorFCV1Factory const& factory)
    : SimulatorFCV1(SimulatorFCV1Storage(factory))
{}

SimulatorFCV1::SimulatorFCV1(SimulatorFCV1Storage const& storage)
    : storage_(storage)
    , world_(b2Vec2_zero)
    , stone_bodies_()
    , stones_dirty_()             // Overwritten by UpdateWithStorage
    , all_stones_stopped_()       // Overwritten later because UpdateWithStorage sets the dirty flag to true
    , all_stones_stopped_dirty_() // Overwritten by UpdateWithStorage
{
    ValidateStones(storage_.stones);

    b2BodyDef stone_body_def;
    stone_body_def.type = b2_dynamicBody;
    stone_body_def.awake = false;
    stone_body_def.bullet = true;
    stone_body_def.enabled = false;

    b2CircleShape stone_shape;
    stone_shape.m_radius = Stone::kRadius;

    b2FixtureDef stone_fixture_def;
    stone_fixture_def.shape = &stone_shape;
    stone_fixture_def.friction = 0.2f;  // Arbitrary, or rather the default value
    stone_fixture_def.restitution = 1.0; // Perfectly elastic collision (there is no evidence for this and it is probably incorrect)
    stone_fixture_def.restitutionThreshold = 0.f;  // Restitution threshold. Restitution applies when collision speed exceeds this value (m/s).
    stone_fixture_def.density = kStoneMass / (b2_pi * Stone::kRadius * Stone::kRadius);  // kg/m^2

    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        stone_body_def.userData.pointer = static_cast<uintptr_t>(i);
        stone_bodies_[i] = world_.CreateBody(&stone_body_def);
        stone_bodies_[i]->CreateFixture(&stone_fixture_def);
    }

    UpdateWithStorage();
}

void SimulatorFCV1::SetStones(ISimulator::AllStones const& stones)
{
    ValidateStones(stones);

    // update bodies
    for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
        SetStoneInner(i, stones[i]);
    }

    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}
void SimulatorFCV1::SetStone(StoneIndex index, std::optional<StoneState> const& stone)
{
    if ((index.GetTeam() != Team::k0 && index.GetTeam() != Team::k1) || index.GetStoneNumber() >= 8)
        throw std::invalid_argument("SimulatorFCV1::SetStone: index is out of range.");
    ValidateStoneState(stone);

    SetStoneInner(index.GetIndex(), stone);
    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

void SimulatorFCV1::Step()
{
    // simulate
    for (auto stone_body : stone_bodies_) {
        b2Vec2 normalized_stone_velocity = stone_body->GetLinearVelocity();
        float stone_speed = normalized_stone_velocity.Normalize();
        float const angular_velocity = stone_body->GetAngularVelocity();

        // Calculate velocity
        // Ignore stopped stones
        constexpr float kGravity = 9.80665f;
        if (stone_speed > std::numeric_limits<float>::epsilon()) {
            float const longitudinal_acceleration = -(0.00200985f / (stone_speed + 0.06385782f) + 0.00626286f) * kGravity;
            float const new_stone_speed = stone_speed + longitudinal_acceleration * kSecondsPerFrame;
            if (new_stone_speed <= 0.f) {
                stone_body->SetLinearVelocity(b2Vec2_zero);
            } else {
                auto yaw_rate = [&]() -> float {
                    if (std::abs(angular_velocity) <= std::numeric_limits<float>::epsilon()) return 0.f;
                    return (angular_velocity > 0.f ? 1.0f : -1.0f) * 0.00820f * std::pow(stone_speed, -0.8f);
                };

                float const yaw = yaw_rate() * kSecondsPerFrame;
                float const longitudinal_velocity = new_stone_speed * std::cos(yaw);
                float const transverse_velocity = new_stone_speed * std::sin(yaw);
                b2Vec2 const& e_longitudinal = normalized_stone_velocity;
                b2Vec2 const e_transverse = e_longitudinal.Skew();
                b2Vec2 const new_stone_velocity = longitudinal_velocity * e_longitudinal + transverse_velocity * e_transverse;
                stone_body->SetLinearVelocity(new_stone_velocity);
            }
        }

        // Calculate angular velocity
        if (std::abs(angular_velocity) > std::numeric_limits<float>::epsilon()) {
            float const angular_accel = -0.025f / std::max(stone_speed, 0.001f) * kSecondsPerFrame;
            float new_angular_velocity = 0.f;
            if (std::abs(angular_velocity) <= std::abs(angular_accel)) {
                new_angular_velocity = 0.f;
            } else {
                new_angular_velocity = angular_velocity + angular_accel * angular_velocity / std::abs(angular_velocity);
            }
            stone_body->SetAngularVelocity(new_angular_velocity);
        }
    }

    world_.Step(
        kSecondsPerFrame,
        8,  // velocityIterations (the official manual recommends 8)
        3); // positionIterations (the official manual recommends 3)

    stones_dirty_ = true;
    all_stones_stopped_dirty_ = true;
}

ISimulator::AllStones SimulatorFCV1::GetStones() const
{
    RefreshStoredStones();
    return storage_.stones;
}

void SimulatorFCV1::RefreshStoredStones() const
{
    if (stones_dirty_) {
        // update stones_
        for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
            if (stone_bodies_[i]->IsEnabled()) {
                ISimulator::StoneState stone;
                stone.position = ToDigitalCurlingVector2(stone_bodies_[i]->GetWorldCenter());
                stone.angle = stone_bodies_[i]->GetAngle();
                stone.translational_velocity = ToDigitalCurlingVector2(stone_bodies_[i]->GetLinearVelocity());
                stone.angular_velocity = stone_bodies_[i]->GetAngularVelocity();
                storage_.stones[i] = stone;
            } else {
                storage_.stones[i] = std::nullopt;
            }
        }
        stones_dirty_ = false;
    }
}

bool SimulatorFCV1::AreAllStonesStopped() const
{
    if (all_stones_stopped_dirty_) {
        all_stones_stopped_ = true;
        for (auto stone_body : stone_bodies_) {
            if (!stone_body->IsEnabled()) continue;

            if (stone_body->GetLinearVelocity().LengthSquared() > std::numeric_limits<float>::epsilon() ||
                std::abs(stone_body->GetAngularVelocity()) > std::numeric_limits<float>::epsilon())
            {
                all_stones_stopped_ = false;
                break;
            }
        }
        all_stones_stopped_dirty_ = false;
    }
    return all_stones_stopped_;
}

float SimulatorFCV1::GetSecondsPerFrame() const
{
    return kSecondsPerFrame;
}

std::unique_ptr<ISimulatorFactory> SimulatorFCV1::GetFactory() const
{
    return std::make_unique<SimulatorFCV1Factory>(storage_.factory);
}

std::unique_ptr<ISimulatorStorage> SimulatorFCV1::CreateStorage() const
{
    RefreshStoredStones();
    return std::make_unique<SimulatorFCV1Storage>(storage_);
}

void SimulatorFCV1::Save(SimulatorFCV1Storage & storage) const
{
    RefreshStoredStones();
    storage = storage_;
}

void SimulatorFCV1::Load(SimulatorFCV1Storage const& storage)
{
    ValidateStones(storage.stones);

    storage_ = storage;
    UpdateWithStorage();
}

moves::Shot SimulatorFCV1::CalculateShot(Vector2 const& target_position, float const target_speed, float const shot_angular_velocity) const {
    if (!std::isfinite(target_speed) || !std::isfinite(target_position.x) || !std::isfinite(target_position.y) ||
        !std::isfinite(shot_angular_velocity))
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: arguments must be finite.");
    if (target_speed < 0.f)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_speed must be non-negative.");
    if (target_speed > 4.f)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_speed is too large.");
    if (shot_angular_velocity == 0.f)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: this function is not supported for zero-spin shots.");

    auto const target_r = target_position.Length();
    if (target_r < 0.1f)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_position is too close to the origin.");
    // The regression is fitted for targets up to the far end of the sheet.
    constexpr float kMaxTargetDistance = 60.f;
    if (target_r > kMaxTargetDistance)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_position is too far from the origin.");

    float const v0_speed = [target_r, target_speed] {
        float c0, c1, c2;
        // The logarithm argument (target_r + kC1[1]) must be positive in every branch.
        auto const check_log_domain = [target_r](float const offset) {
            if (!(target_r + offset > 0.f))
                throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_position is too close to the origin for the target_speed.");
        };
        if (target_speed <= 0.05f) {
            float constexpr kC0[] = { 0.0005048122574925176,0.2756242531609261 };
            float constexpr kC1[] = { 0.00046669575066030805,-29.898958358378636,-0.0014030973174948508 };
            check_log_domain(kC1[1]);
            float constexpr kC2[] = { 0.13968687866736632,0.41120940058777616 };

            c0 = kC0[0] * target_r + kC0[1];
            c1 = -kC1[0] * std::log(target_r + kC1[1]) + kC1[2];
            c2 = kC2[0] * target_r + kC2[1];
        } else if (target_speed <= 1.f) {
            float constexpr kC0[] = { -0.0014309170115803444,0.9858457898438147 };
            float constexpr kC1[] = { -0.0008339331735471273,-29.86751291726946,-0.19811799977982522 };
            check_log_domain(kC1[1]);
            float constexpr kC2[] = { 0.13967323742978,0.42816312110477517 };

            c0 = kC0[0] * target_r + kC0[1];
            c1 = -kC1[0] * std::log(target_r + kC1[1]) + kC1[2];
            c2 = kC2[0] * target_r + kC2[1];
        } else {
            float constexpr kC0[] = { 1.0833113118071224e-06,-0.00012132851917870833,0.004578093297561233,0.9767006869364527 };
            float constexpr kC1[] = { 0.07950648211492622,-8.228225657195706,-0.05601306077702578 };
            check_log_domain(kC1[1]);
            float constexpr kC2[] = { 0.14140440186382008,0.3875782508767419 };

            c0 = kC0[0] * target_r * target_r * target_r + kC0[1] * target_r * target_r + kC0[2] * target_r + kC0[3];
            c1 = -kC1[0] * std::log(target_r + kC1[1]) + kC1[2];
            c2 = kC2[0] * target_r + kC2[1];
        }
        float const radicand = c0 * target_speed * target_speed + c1 * target_speed + c2;
        if (!(radicand > 0.f))
            throw std::invalid_argument("SimulatorFCV1::CalculateShot: no shot is available for the target.");
        float const v0 = std::sqrt(radicand);
        if (!std::isfinite(v0))
            throw std::invalid_argument("SimulatorFCV1::CalculateShot: no shot is available for the target.");
        return v0;
    }();

    if (target_speed >= v0_speed)
        throw std::invalid_argument("SimulatorFCV1::CalculateShot: target_speed is too large for the target_position.");

    constexpr float half_pi = 1.57079632679f;
    float const angular_velocity = shot_angular_velocity > 0 ? half_pi : -half_pi;
    Vector2 const delta = [this, angular_velocity, v0_speed, target_speed] {
        thread_local std::unique_ptr<IStepSimulator> s_simulator;
        if (!s_simulator) {
            auto created = this->GetFactory()->CreateSimulator();
            auto* step_simulator = dynamic_cast<IStepSimulator*>(created.get());
            if (!step_simulator)
                throw std::logic_error("SimulatorFCV1::CalculateShot: the factory did not create a step simulator.");
            created.release();
            s_simulator.reset(step_simulator);
        }

        ISimulator::AllStones init_stones;
        init_stones[0].emplace(Vector2(), 0.f, Vector2(0.f, v0_speed), angular_velocity);
        s_simulator->SetStones(init_stones);

        int frames = 0;
        while (!s_simulator->AreAllStonesStopped()) {
            if (frames >= simulators::kMaxSimulationFrames)
                throw std::runtime_error("SimulatorFCV1::CalculateShot: simulation did not finish within the frame limit.");
            ++frames;

            auto const stones = s_simulator->GetStones();
            auto const speed = stones[0]->translational_velocity.Length();
            if (speed <= target_speed) return stones[0]->position;
            s_simulator->Step();
        }

        return s_simulator->GetStones()[0]->position;
    }();

    float const delta_angle = std::atan2(delta.x, delta.y); // Note: delta.x, delta.y is the intended order
    float const target_angle = std::atan2(target_position.y, target_position.x);
    float const v0_angle = target_angle + delta_angle; // Shot direction
    return moves::Shot { v0_speed, angular_velocity, v0_angle };
}

void SimulatorFCV1::UpdateWithStorage()
{
    SetStones(storage_.stones);
    stones_dirty_ = false;  // storage_.stones and Box2D data are already synchronized.
    // all_stones_dirty_ = true is already set inside SetStones()
}

} // namespace digitalcurling::simulators
