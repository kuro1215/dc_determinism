// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>
#include <gtest/gtest.h>

#include "digitalcurling/plugins/detail/simulator_wasm_trajectory.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_simulator_storage.hpp"

namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins::detail;

class CountingSimulator final : public simulators::IStepSimulator {
public:
    explicit CountingSimulator(int stop_after) : stop_after_(stop_after)
    {
        stones_[0].emplace(Vector2(0.f, 1.f), 0.f, Vector2(), 0.f);
    }

    const char* GetId() const noexcept override { return "counting"; }
    void SetStones(AllStones const& stones) override { stones_ = stones; }
    void SetStone(StoneIndex index, std::optional<StoneState> const& stone) override
    {
        stones_[index.GetIndex()] = stone;
    }
    void Step() override
    {
        ++steps_;
        stones_[0]->angle = static_cast<float>(steps_);
    }
    AllStones GetStones() const override { return stones_; }
    bool AreAllStonesStopped() const override { return steps_ >= stop_after_; }
    float GetSecondsPerFrame() const override { return 0.001f; }
    std::unique_ptr<simulators::ISimulatorFactory> GetFactory() const override
    {
        throw std::logic_error("Counting simulator has no factory");
    }
    std::unique_ptr<simulators::ISimulatorStorage> CreateStorage() const override { return {}; }
    void Save(simulators::ISimulatorStorage&) const override {}
    void Load(simulators::ISimulatorStorage const&) override {}
    int Steps() const { return steps_; }

private:
    int stop_after_;
    int steps_ = 0;
    AllStones stones_{};
};

// A simulator that cannot step: it only implements ISimulator::Simulate().
class SimulateOnlySimulator final : public simulators::ISimulator {
public:
    const char* GetId() const noexcept override { return "simulate-only"; }
    void SetStones(AllStones const& stones) override { stones_ = stones; }
    void SetStone(StoneIndex index, std::optional<StoneState> const& stone) override
    {
        stones_[index.GetIndex()] = stone;
    }
    void Simulate(float) override { ++simulate_calls_; }
    AllStones GetStones() const override { return stones_; }
    std::unique_ptr<simulators::ISimulatorFactory> GetFactory() const override
    {
        throw std::logic_error("Simulate-only simulator has no factory");
    }
    std::unique_ptr<simulators::ISimulatorStorage> CreateStorage() const override { return {}; }
    void Save(simulators::ISimulatorStorage&) const override {}
    void Load(simulators::ISimulatorStorage const&) override {}
    int SimulateCalls() const { return simulate_calls_; }

private:
    int simulate_calls_ = 0;
    AllStones stones_{};
};

TEST(WasmTrajectory, FrameIntervalsAreValidatedBeforeConversion)
{
    int interval = 0;
    EXPECT_TRUE(TryWasmFrameInterval(1.0, interval));
    EXPECT_EQ(interval, 1);
    EXPECT_TRUE(TryWasmFrameInterval(1000.0, interval));
    EXPECT_EQ(interval, 1000);

    for (double invalid : {0.0, -1.0, 1.5, 1001.0, 2147483648.0,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_FALSE(TryWasmFrameInterval(invalid, interval));
    }
}

TEST(WasmTrajectory, PreservesInitialAndFinalFramesForSampling)
{
    for (int interval : {1, 10}) {
        CountingSimulator simulator(11);
        auto const result = SimulateWasmTrajectory(simulator, interval, coordinate::kSheetWidthMax);
        EXPECT_EQ(result.count, interval == 1 ? 12 : 3);
        ASSERT_EQ(result.trajectory.size(), static_cast<std::size_t>(result.count) * kWasmBoardValueCount);
        EXPECT_EQ(result.trajectory[2], 0.f);
        EXPECT_EQ(result.trajectory[result.trajectory.size() - kWasmBoardValueCount + 2], 11.f);
        EXPECT_EQ(simulator.Steps(), 11);
    }
}

TEST(WasmTrajectory, EnforcesStepBudgetWithoutReturningPartialOutput)
{
    CountingSimulator stops_at_limit(kWasmMaxSimulationSteps);
    auto const complete = SimulateWasmTrajectory(stops_at_limit, 1000, coordinate::kSheetWidthMax);
    EXPECT_EQ(complete.count, 61);
    EXPECT_EQ(stops_at_limit.Steps(), kWasmMaxSimulationSteps);
    EXPECT_EQ(complete.trajectory.size(), static_cast<std::size_t>(complete.count) * kWasmBoardValueCount);

    CountingSimulator never_stops(std::numeric_limits<int>::max());
    auto const exhausted = SimulateWasmTrajectory(never_stops, 1000, coordinate::kSheetWidthMax);
    EXPECT_EQ(exhausted.count, kWasmBudgetExhausted);
    EXPECT_TRUE(exhausted.trajectory.empty());
    EXPECT_EQ(never_stops.Steps(), kWasmMaxSimulationSteps);

    CountingSimulator invalid_batch(1);
    auto const invalid = SimulateWasmTrajectory(invalid_batch, 1001, coordinate::kSheetWidthMax);
    EXPECT_EQ(invalid.count, -4);
    EXPECT_TRUE(invalid.trajectory.empty());
    EXPECT_EQ(invalid_batch.Steps(), 0);
}

TEST(WasmTrajectory, SimulateOnlySimulatorIsReportedAsUnsupported)
{
    SimulateOnlySimulator simulator;
    simulators::ISimulator& base = simulator;
    auto const result = SimulateWasmTrajectory(base, 1, coordinate::kSheetWidthMax);
    EXPECT_EQ(result.count, kWasmUnsupportedSimulator);
    EXPECT_EQ(result.count, -7);
    EXPECT_TRUE(result.trajectory.empty());
    // The trajectory path must not fall back to Simulate().
    EXPECT_EQ(simulator.SimulateCalls(), 0);
}

TEST(WasmTrajectory, StepSimulatorBehindBaseInterfaceKeepsItsTrajectory)
{
    CountingSimulator direct(11);
    CountingSimulator through_base(11);
    simulators::ISimulator& base = through_base;

    auto const expected = SimulateWasmTrajectory(direct, 1, coordinate::kSheetWidthMax);
    auto const actual = SimulateWasmTrajectory(base, 1, coordinate::kSheetWidthMax);
    EXPECT_EQ(actual.count, expected.count);
    EXPECT_EQ(actual.trajectory, expected.trajectory);
}

TEST(WasmTrajectory, RejectsOutputAndFrameCountOverflow)
{
    EXPECT_TRUE(CanAppendWasmFrame(kWasmMaxTrajectoryValues - kWasmBoardValueCount, 0));
    EXPECT_FALSE(CanAppendWasmFrame(kWasmMaxTrajectoryValues - kWasmBoardValueCount + 1, 0));
    EXPECT_FALSE(CanAppendWasmFrame(std::numeric_limits<std::size_t>::max(), 0));
    EXPECT_FALSE(CanAppendWasmFrame(0, std::numeric_limits<int>::max()));
    EXPECT_FALSE(CanAppendWasmFrame(0, -1));
}

} // namespace
