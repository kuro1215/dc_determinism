// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines SimulatorFCV1

#pragma once

#include <array>
#include <memory>
#include <vector>
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

#include "box2d_util.hpp"
#include "simulator_fcv1_factory.hpp"
#include "simulator_fcv1_storage.hpp"

namespace digitalcurling::simulators {


/// @brief Friction-CurlVelocity simulator, version 1
class SimulatorFCV1 : public IStepSimulator {
public:
    /// @brief Stone mass [kg]
    static constexpr float kStoneMass = 19.96f;

    /// @brief Elapsed time per Step() call [s]
    static constexpr float kSecondsPerFrame = 0.001f;

    /// @brief Constructor
    /// @param factory Factory for this simulator
    explicit SimulatorFCV1(SimulatorFCV1Factory const& factory);
    /// @brief Constructor
    /// @param storage Storage for this simulator
    explicit SimulatorFCV1(SimulatorFCV1Storage const& storage);
    virtual ~SimulatorFCV1() = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }

    virtual void SetStones(ISimulator::AllStones const& stones) override;
    virtual void SetStone(StoneIndex index, std::optional<StoneState> const& stone) override;
    virtual void Step() override;

    virtual bool AreAllStonesStopped() const override;
    virtual ISimulator::AllStones GetStones() const override;

    virtual std::unique_ptr<ISimulatorFactory> GetFactory() const override;
    virtual float GetSecondsPerFrame() const override;

    virtual std::unique_ptr<ISimulatorStorage> CreateStorage() const override;
    void Save(SimulatorFCV1Storage & storage) const;
    void Load(SimulatorFCV1Storage const& storage);

    virtual void Save(ISimulatorStorage & storage) const override {
        Save(dynamic_cast<SimulatorFCV1Storage &>(storage));
    }
    virtual void Load(ISimulatorStorage const& storage) override {
        Load(dynamic_cast<SimulatorFCV1Storage const&>(storage));
    }

    /// @brief Estimate a shot passing through the specified position at the specified speed
    /// @param target_position Target position
    /// @param target_speed Speed upon reaching the target position
    /// @param shot_angular_velocity Shot angular velocity
    /// @return Estimated shot
    /// @note - This function simulates one shot using FCV1, so its execution is not fast.
    /// @note - This function is derived by regression from simulation results rather than analytically, so some error exists, especially in travel distance.
    virtual moves::Shot CalculateShot(Vector2 const& target_position, float const target_speed, float const shot_angular_velocity) const;

private:
    mutable SimulatorFCV1Storage storage_;
    b2World world_;
    std::array<b2Body*, StoneCoordinate::kStoneMax> stone_bodies_;
    mutable bool stones_dirty_;
    mutable bool all_stones_stopped_;
    mutable bool all_stones_stopped_dirty_;

    // Apply storage data to internal state
    void UpdateWithStorage();

    // Refresh storage_.stones from the Box2D bodies when they changed
    void RefreshStoredStones() const;

    inline void SetStoneInner(int index, std::optional<StoneState> const& stone) {
        if (stone.has_value()) {
            stone_bodies_[index]->SetEnabled(true);
            stone_bodies_[index]->SetAwake(true);
            stone_bodies_[index]->SetTransform(ToB2Vec2(stone->position), stone->angle);
            stone_bodies_[index]->SetLinearVelocity(ToB2Vec2(stone->translational_velocity));
            stone_bodies_[index]->SetAngularVelocity(stone->angular_velocity);
        } else {
            stone_bodies_[index]->SetEnabled(false);
        }
    }
};

} // namespace digitalcurling::simulators
