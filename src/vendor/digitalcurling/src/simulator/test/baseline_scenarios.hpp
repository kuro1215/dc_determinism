// Representative scenarios whose exact simulation results were recorded at the
// base commit 2c7ebc9 (before `Simulate()` replaced the mode-based API).
// `SimulateBaseline*` tests compare against `baseline_expected.hpp`.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#include "common.hpp"

namespace digitalcurling::test {

struct BaselineScenario {
    char const* name;
    float sheet_width;
    dcs::ISimulator::AllStones initial;
};

/// Raw IEEE-754 bit patterns of one stone: position.x, position.y, angle,
/// translational_velocity.x, translational_velocity.y, angular_velocity.
struct BaselineStoneBits {
    bool present;
    std::array<std::uint32_t, 6> bits;
};

using BaselineBits = std::array<BaselineStoneBits, 16>;

inline std::uint32_t FloatBits(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline BaselineBits ToBaselineBits(dcs::ISimulator::AllStones const& stones)
{
    BaselineBits result{};
    for (std::size_t i = 0; i < stones.size(); ++i) {
        if (!stones[i].has_value()) {
            continue;
        }
        auto const& s = *stones[i];
        result[i].present = true;
        result[i].bits = {
            FloatBits(s.position.x),
            FloatBits(s.position.y),
            FloatBits(s.angle),
            FloatBits(s.translational_velocity.x),
            FloatBits(s.translational_velocity.y),
            FloatBits(s.angular_velocity)};
    }
    return result;
}

/// Compares raw bit patterns and reports the first difference.
inline ::testing::AssertionResult BitsEqual(BaselineBits const& actual, BaselineBits const& expected)
{
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i].present != expected[i].present) {
            return ::testing::AssertionFailure()
                << "stone " << i << " is " << (actual[i].present ? "present" : "absent")
                << " but was recorded as " << (expected[i].present ? "present" : "absent");
        }
        if (!actual[i].present) {
            continue;
        }
        for (std::size_t k = 0; k < actual[i].bits.size(); ++k) {
            if (actual[i].bits[k] != expected[i].bits[k]) {
                return ::testing::AssertionFailure()
                    << "stone " << i << " field " << k << " is 0x" << std::hex << actual[i].bits[k]
                    << " but was recorded as 0x" << expected[i].bits[k];
            }
        }
    }
    return ::testing::AssertionSuccess();
}

/// Independent copy of the pre-change `SimulatorSimulate(Full, 0, width)` loop.
/// It deliberately does not use `StepUntilRest()`.
inline void ManualStepUntilRest(dcs::IStepSimulator& simulator, float sheet_width)
{
    float const width = dc::coordinate::IsValidSheetWidth(sheet_width) ? sheet_width : dc::coordinate::kSheetWidthMax;
    while (!simulator.AreAllStonesStopped()) {
        simulator.Step();
        auto const stones = simulator.GetStones();
        for (int i = 0; i < dc::StoneCoordinate::kStoneMax; ++i) {
            if (stones[i].has_value() && !stones[i]->IsValidOnSimulation(width)) {
                simulator.SetStone(dc::StoneIndex(i), std::nullopt);
            }
        }
    }
}

inline dcs::ISimulator::StoneState BaselineStone(
    float x, float y, float vx, float vy, float angular_velocity, float angle = 0.f)
{
    return dcs::ISimulator::StoneState(
        dc::Vector2(x, y), angle, dc::Vector2(vx, vy), angular_velocity);
}

inline std::vector<BaselineScenario> const& BaselineScenarios()
{
    static std::vector<BaselineScenario> const scenarios = [] {
        std::vector<BaselineScenario> list;
        constexpr float kWidth = dc::coordinate::kSheetWidthMax;

        {   // A draw that stops inside the house.
            BaselineScenario s{"draw", kWidth, {}};
            s.initial[0] = BaselineStone(0.f, 2.f, 0.02f, 2.3f, 1.57f);
            list.push_back(s);
        }
        {   // A takeout with collisions against a guard and a shot stone.
            BaselineScenario s{"takeout", kWidth, {}};
            s.initial[0] = BaselineStone(0.01f, 20.f, 0.f, 3.f, 0.f);
            s.initial[1] = BaselineStone(0.f, 36.f, 0.f, 0.f, 0.f);
            s.initial[2] = BaselineStone(0.3f, 38.2f, 0.f, 0.f, 0.f);
            s.initial[3] = BaselineStone(-0.4f, 38.9f, 0.f, 0.f, 0.f);
            list.push_back(s);
        }
        {   // A stone that leaves the sheet sideways.
            BaselineScenario s{"side_out", kWidth, {}};
            s.initial[0] = BaselineStone(0.f, 10.f, 3.5f, 1.f, 1.f);
            s.initial[1] = BaselineStone(0.5f, 38.f, 0.f, 0.f, 0.f);
            list.push_back(s);
        }
        {   // A stone that leaves the sheet past the back line.
            BaselineScenario s{"back_out", kWidth, {}};
            s.initial[0] = BaselineStone(0.05f, 20.f, 0.f, 5.f, -1.f);
            list.push_back(s);
        }
        {   // A multi-stone cluster in and around the house.
            BaselineScenario s{"cluster", kWidth, {}};
            s.initial[0] = BaselineStone(0.f, 38.4f, 0.f, 0.f, 0.f);
            s.initial[1] = BaselineStone(0.29f, 38.5f, 0.f, 0.f, 0.f);
            s.initial[2] = BaselineStone(-0.29f, 38.6f, 0.f, 0.f, 0.f);
            s.initial[3] = BaselineStone(0.15f, 38.9f, 0.f, 0.f, 0.f);
            s.initial[4] = BaselineStone(-0.15f, 39.1f, 0.f, 0.f, 0.f);
            s.initial[5] = BaselineStone(0.f, 37.9f, 0.f, 0.f, 0.f);
            s.initial[6] = BaselineStone(0.6f, 38.0f, 0.f, 0.f, 0.f);
            s.initial[8] = BaselineStone(0.05f, 28.f, -0.01f, 3.6f, 1.2f);
            s.initial[9] = BaselineStone(-0.2f, 15.f, 0.1f, 3.f, -1.f);
            list.push_back(s);
        }
        {   // A narrower sheet removes a stone that the widest sheet keeps.
            BaselineScenario s{"narrow_sheet", dc::coordinate::kSheetWidthMin, {}};
            s.initial[0] = BaselineStone(1.5f, 12.f, 0.4f, 1.8f, 0.5f);
            s.initial[1] = BaselineStone(-1.6f, 30.f, 0.f, 0.f, 0.f);
            list.push_back(s);
        }
        {   // An invalid width falls back to the maximum sheet width.
            BaselineScenario s{"invalid_width", std::numeric_limits<float>::quiet_NaN(), {}};
            s.initial[0] = BaselineStone(0.f, 10.f, 3.5f, 1.f, 1.f);
            s.initial[1] = BaselineStone(-0.2f, 25.f, 0.f, 3.2f, 0.7f);
            list.push_back(s);
        }
        return list;
    }();
    return scenarios;
}

} // namespace digitalcurling::test
