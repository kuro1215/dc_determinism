// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IStepSimulator and StepUntilRest

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/stone_coordinate.hpp"
#include "digitalcurling/stone_index.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"

namespace digitalcurling::simulators {

/// @brief Absolute upper bound on the frames advanced by one `StepUntilRest()` call
///
/// 10 times the longest legitimate shot (about 60 s) at 0.001 s per frame.
/// A simulation whose stones are still moving at this frame count is aborted with
/// `std::runtime_error`, which bounds the work done for untrusted input
/// (for example a stone with a huge angular velocity).
inline constexpr int kMaxSimulationFrames = 600000;

/// @brief Interface for a simulator that can advance frame by frame
///
/// `ISimulator` only requires `Simulate()`. A simulator that can also expose its
/// time step implements this interface. `Simulate()` is implemented here in
/// terms of `Step()` (see `StepUntilRest()`); an implementer may override it.
///
/// @note
/// The thread-safety contract of `ISimulator` applies: callers must serialize all calls
/// on one instance, including `StepUntilRest()`.
///
/// @sa ISimulator
class IStepSimulator : public ISimulator {
protected:
    /// @brief Default constructor
    IStepSimulator() = default;
    /// @brief Copy constructor
    IStepSimulator(IStepSimulator const&) = default;
    /// @brief Copy assignment operator
    IStepSimulator& operator = (IStepSimulator const&) = default;

public:
    virtual ~IStepSimulator() override = default;

    /// @brief Advances one frame
    ///
    /// Each call advances the simulator's time by `GetSecondsPerFrame()`.
    ///
    /// A native `Step()` does not remove stones that left the playable region
    /// (`StoneState::IsValidOnSimulation()`); `Simulate()` and `StepUntilRest()` do that
    /// after each frame. A simulator may still keep an internal safety sweep
    /// for stones far outside the raw sheet bounds.
    virtual void Step() = 0;

    /// @brief Checks whether all stones have stopped
    /// @returns `true` if all stones have stopped
    virtual bool AreAllStonesStopped() const = 0;

    /// @brief Gets the time advanced by one `Step()` call
    /// @returns Time advanced by one `Step()` call (seconds)
    virtual float GetSecondsPerFrame() const = 0;

    /// @brief Simulates until every stone is at rest using `Step()`
    ///
    /// Equivalent to `StepUntilRest(*this, 0, sheet_width)`.
    ///
    /// @param[in] sheet_width Sheet width (m)
    virtual void Simulate(float sheet_width) override;
};

/// @brief Steps a simulator until every stone is at rest or a frame limit is reached
///
/// After each `Step()`, stones for which `StoneState::IsValidOnSimulation(sheet_width)`
/// is `false` are removed with `SetStone(index, std::nullopt)` in ascending index order.
///
/// @param[in,out] simulator Simulator to advance
/// @param[in] max_frames Maximum number of frames. A value `<= 0` means unbounded
/// @param[in] sheet_width Sheet width (m). A value for which
///   `coordinate::IsValidSheetWidth()` is `false` is treated as `coordinate::kSheetWidthMax`
/// @returns Number of frames advanced (saturated at `INT_MAX`)
/// @throws std::runtime_error if stones are still moving after `kMaxSimulationFrames`
///   frames (including when `max_frames` is unbounded or larger than that limit)
inline int StepUntilRest(IStepSimulator& simulator, int max_frames, float sheet_width)
{
    auto const width = coordinate::IsValidSheetWidth(sheet_width) ? sheet_width : coordinate::kSheetWidthMax;

    std::int64_t frames = 0;
    while (!simulator.AreAllStonesStopped()) {
        if (frames >= kMaxSimulationFrames) {
            throw std::runtime_error("StepUntilRest: simulation did not finish within the frame limit.");
        }

        simulator.Step();
        ++frames;

        auto const stones = simulator.GetStones();
        for (int i = 0; i < StoneCoordinate::kStoneMax; i++) {
            auto const& stone = stones[i];
            if (stone.has_value() && !stone->IsValidOnSimulation(width)) {
                simulator.SetStone(StoneIndex(i), std::nullopt);
            }
        }

        if (max_frames > 0 && frames >= max_frames) {
            break;
        }
    }
    return static_cast<int>(std::min<std::int64_t>(frames, std::numeric_limits<int>::max()));
}

inline void IStepSimulator::Simulate(float sheet_width)
{
    StepUntilRest(*this, 0, sheet_width);
}

} // namespace digitalcurling::simulators
