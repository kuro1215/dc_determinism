// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <optional>
#include <vector>
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"
#include "fixed_types.hpp"
#include "simulator_fcv1plus_factory.hpp"
#include "simulator_fcv1plus_storage.hpp"

namespace digitalcurling::simulators {

class SimulatorFCV1Plus : public IStepSimulator {
public:
    static constexpr float kStoneMass = 19.96f;

    // The wide pow safety proof in the approved design depends on dt=0.001.
    static constexpr float kSecondsPerFrame = 0.001f;

    explicit SimulatorFCV1Plus(SimulatorFCV1PlusFactory const& factory);
    explicit SimulatorFCV1Plus(SimulatorFCV1PlusStorage const& storage);
    ~SimulatorFCV1Plus() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }

    void SetStones(AllStones const& stones) override;
    void SetStone(StoneIndex index, std::optional<StoneState> const& stone) override;
    void Step() override;

    bool AreAllStonesStopped() const override;
    AllStones GetStones() const override;

    std::unique_ptr<ISimulatorFactory> GetFactory() const override;
    float GetSecondsPerFrame() const override;

    std::unique_ptr<ISimulatorStorage> CreateStorage() const override;
    void Save(SimulatorFCV1PlusStorage& storage) const;
    void Load(SimulatorFCV1PlusStorage const& storage);
    void Save(ISimulatorStorage& storage) const override
    {
        Save(dynamic_cast<SimulatorFCV1PlusStorage&>(storage));
    }
    void Load(ISimulatorStorage const& storage) override
    {
        Load(dynamic_cast<SimulatorFCV1PlusStorage const&>(storage));
    }

    moves::Shot CalculateShot(
        Vector2 const& target_position,
        float target_speed,
        float shot_angular_velocity) const;

    // Test-only value accessor. The returned fields have a fixed order and
    // contain Q8.24 two's-complement raw values, independent of struct layout.
    SimulatorFCV1PlusStorage::RawStones GetRawStonesForTesting() const;

private:
    mutable SimulatorFCV1PlusStorage storage_;
    fcv1plus_detail::AllStonesFixed stones_fixed_;
    mutable AllStones stones_cache_;
    mutable bool stones_dirty_;
    mutable bool all_stones_stopped_;
    mutable bool all_stones_stopped_dirty_;

    void SetStoneInner(int index, std::optional<StoneState> const& stone);
    void UpdateStorageRawStones() const;
};

} // namespace digitalcurling::simulators
