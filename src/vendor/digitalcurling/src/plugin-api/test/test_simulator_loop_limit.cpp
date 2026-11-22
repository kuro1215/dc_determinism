#include <cstdlib>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <gtest/gtest.h>

#define DIGITALCURLING_PLUGIN_NAME "loop-limit-test"
#include "digitalcurling/plugins/detail/simulator_capi_exports.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins::detail;

class LoopLimitSimulator final : public simulators::IStepSimulator {
public:
    explicit LoopLimitSimulator(int stop_after) : stop_after_(stop_after)
    {
        stones_[0].emplace(Vector2(0.f, 1.f), 0.f, Vector2(), 0.f);
    }

    const char* GetId() const noexcept override { return "loop_limit"; }
    void SetStones(AllStones const& stones) override { stones_ = stones; }
    void SetStone(StoneIndex index, std::optional<StoneState> const& stone) override
    {
        stones_[index.GetIndex()] = stone;
    }
    void Step() override { ++steps_; }
    AllStones GetStones() const override { return stones_; }
    bool AreAllStonesStopped() const override { return steps_ >= stop_after_; }
    float GetSecondsPerFrame() const override { return 0.001f; }
    std::unique_ptr<simulators::ISimulatorFactory> GetFactory() const override
    {
        throw std::logic_error("LoopLimitSimulator has no factory");
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

TEST(SimulatorLoopLimit, StepUntilRestThrowsWhenStonesNeverStop)
{
    LoopLimitSimulator never_stops(std::numeric_limits<int>::max());
    EXPECT_THROW(
        simulators::StepUntilRest(never_stops, 0, coordinate::kSheetWidthMax),
        std::runtime_error);
    EXPECT_EQ(never_stops.Steps(), simulators::kMaxSimulationFrames);
}

TEST(SimulatorLoopLimit, DefaultSimulateThrowsWhenStonesNeverStop)
{
    LoopLimitSimulator never_stops(std::numeric_limits<int>::max());
    EXPECT_THROW(never_stops.Simulate(coordinate::kSheetWidthMax), std::runtime_error);
    EXPECT_EQ(never_stops.Steps(), simulators::kMaxSimulationFrames);
}

TEST(SimulatorLoopLimit, BoundsLargeFrameRequests)
{
    LoopLimitSimulator never_stops(std::numeric_limits<int>::max());
    EXPECT_THROW(
        simulators::StepUntilRest(never_stops, std::numeric_limits<int>::max(), coordinate::kSheetWidthMax),
        std::runtime_error);
    EXPECT_EQ(never_stops.Steps(), simulators::kMaxSimulationFrames);
}

TEST(SimulatorLoopLimit, DoesNotAffectSimulationsWithinTheLimit)
{
    LoopLimitSimulator stops_at_limit(simulators::kMaxSimulationFrames);
    EXPECT_EQ(
        simulators::StepUntilRest(stops_at_limit, 0, coordinate::kSheetWidthMax),
        simulators::kMaxSimulationFrames);

    LoopLimitSimulator limited_at_limit(std::numeric_limits<int>::max());
    EXPECT_EQ(
        simulators::StepUntilRest(limited_at_limit, simulators::kMaxSimulationFrames, coordinate::kSheetWidthMax),
        simulators::kMaxSimulationFrames);

    LoopLimitSimulator short_run(10);
    EXPECT_EQ(simulators::StepUntilRest(short_run, 0, coordinate::kSheetWidthMax), 10);
}

TEST(SimulatorLoopLimit, ExportedSimulateReportsTheFrameLimit)
{
    LoopLimitSimulator never_stops(std::numeric_limits<int>::max());
    char* error = nullptr;
    EXPECT_EQ(
        SimulatorSimulateImpl<LoopLimitSimulator>(&never_stops, coordinate::kSheetWidthMax, &error),
        DIGITALCURLING_ERR_OTHER_EXCEPTION);
    ASSERT_NE(error, nullptr);
    EXPECT_NE(std::string(error).find("frame limit"), std::string::npos);
    std::free(error);
    EXPECT_EQ(never_stops.Steps(), simulators::kMaxSimulationFrames);
}

TEST(SimulatorLoopLimit, ExportedStepReportsTheFrameLimit)
{
    LoopLimitSimulator never_stops(std::numeric_limits<int>::max());
    char* error = nullptr;
    EXPECT_EQ(
        SimulatorStepImpl<LoopLimitSimulator>(
            &never_stops, std::numeric_limits<int>::max(), coordinate::kSheetWidthMax, &error),
        DIGITALCURLING_ERR_OTHER_EXCEPTION);
    ASSERT_NE(error, nullptr);
    std::free(error);
    EXPECT_EQ(never_stops.Steps(), simulators::kMaxSimulationFrames);

    LoopLimitSimulator short_run(10);
    EXPECT_EQ(
        SimulatorStepImpl<LoopLimitSimulator>(&short_run, 5, coordinate::kSheetWidthMax, &error),
        DIGITALCURLING_OK);
    EXPECT_EQ(short_run.Steps(), 5);
}

} // namespace
