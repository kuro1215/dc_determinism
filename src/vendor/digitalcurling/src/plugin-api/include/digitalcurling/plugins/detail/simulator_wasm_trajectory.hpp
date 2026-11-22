// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

namespace digitalcurling::plugins::detail {

inline constexpr int kWasmMaxBatchSteps = 1000;
inline constexpr int kWasmMaxSimulationSteps = 60000;
inline constexpr std::size_t kWasmMaxTrajectoryBytes = 16 * 1024 * 1024;
inline constexpr std::size_t kWasmMaxTrajectoryValues = kWasmMaxTrajectoryBytes / sizeof(float);
inline constexpr std::size_t kWasmBoardValueCount = 3 * StoneCoordinate::kStoneMax;
inline constexpr int kWasmBudgetExhausted = -6;
// The simulator cannot step, so no trajectory can be produced for it.
inline constexpr int kWasmUnsupportedSimulator = -7;

struct WasmTrajectory {
    int count;
    std::vector<float> trajectory;
};

inline bool TryWasmFrameInterval(double value, int& interval)
{
    if (!std::isfinite(value) || value < 1.0 || value > kWasmMaxBatchSteps ||
        value != std::floor(value)) {
        return false;
    }

    interval = static_cast<int>(value);
    return true;
}

inline bool CanAppendWasmFrame(std::size_t values, int frames)
{
    return frames >= 0 && frames < std::numeric_limits<int>::max() &&
        values <= kWasmMaxTrajectoryValues - kWasmBoardValueCount;
}

inline void AppendWasmFrame(
    simulators::ISimulator::AllStones const& stones, std::vector<float>& trajectory)
{
    for (auto const& stone : stones) {
        if (stone) {
            trajectory.push_back(stone->position.x);
            trajectory.push_back(stone->position.y);
            trajectory.push_back(stone->angle);
        } else {
            trajectory.push_back(0.f);
            trajectory.push_back(0.f);
            trajectory.push_back(0.f);
        }
    }
}

inline WasmTrajectory SimulateWasmTrajectory(
    simulators::IStepSimulator& simulator, int frame_interval, float sheet_width)
{
    if (frame_interval < 1 || frame_interval > kWasmMaxBatchSteps) {
        return {-4, {}};
    }

    WasmTrajectory result{0, {}};
    if (!CanAppendWasmFrame(result.trajectory.size(), result.count)) {
        return {kWasmBudgetExhausted, {}};
    }
    AppendWasmFrame(simulator.GetStones(), result.trajectory);
    ++result.count;

    int total_steps = 0;
    while (!simulator.AreAllStonesStopped()) {
        if (total_steps >= kWasmMaxSimulationSteps) {
            return {kWasmBudgetExhausted, {}};
        }
        const int batch = std::min(frame_interval, kWasmMaxSimulationSteps - total_steps);
        const int completed = simulators::StepUntilRest(simulator, batch, sheet_width);
        if (completed <= 0 || completed > batch) {
            return {kWasmBudgetExhausted, {}};
        }
        total_steps += completed;

        if (!CanAppendWasmFrame(result.trajectory.size(), result.count)) {
            return {kWasmBudgetExhausted, {}};
        }
        AppendWasmFrame(simulator.GetStones(), result.trajectory);
        ++result.count;
    }
    return result;
}

// A trajectory needs frame-by-frame stepping. A simulator that implements only
// ISimulator::Simulate() is reported as unsupported.
inline WasmTrajectory SimulateWasmTrajectory(
    simulators::ISimulator& simulator, int frame_interval, float sheet_width)
{
    auto* step_simulator = dynamic_cast<simulators::IStepSimulator*>(&simulator);
    if (!step_simulator) {
        return {kWasmUnsupportedSimulator, {}};
    }
    return SimulateWasmTrajectory(*step_simulator, frame_interval, sheet_width);
}

} // namespace digitalcurling::plugins::detail
