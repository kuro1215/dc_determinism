// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include "digitalcurling/plugins/loader_types.h"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

namespace digitalcurling::test {

std::string GetSnapshotData(DigitalCurling_HandleId id, size_t snapshot_size);

/// Converts a simulator created through a plugin to its step interface.
/// Throws std::logic_error (a test failure) when the simulator cannot step.
inline std::unique_ptr<simulators::IStepSimulator> ToStepSimulator(std::unique_ptr<simulators::ISimulator> simulator)
{
    auto* step_simulator = dynamic_cast<simulators::IStepSimulator*>(simulator.get());
    if (!step_simulator) {
        throw std::logic_error("ToStepSimulator: the simulator does not implement IStepSimulator.");
    }
    simulator.release();
    return std::unique_ptr<simulators::IStepSimulator>(step_simulator);
}

/// Checks the C API with a step-capable simulator plugin: dc_loader_simulator_simulate() runs until
/// every stone is at rest, removes stones that left the sheet, and gives the same stones as stepping.
void CheckLoaderSimulate(const char* simulator_plugin_name);

/// Checks the C++ wrappers of a step-capable, invertible simulator plugin: Simulate() gives the same
/// stones as stepping, and the values returned by GetStones() and GetFactory() stay valid.
void CheckWrapperSimulate(simulators::ISimulatorFactory const& factory);

/// Runs the recorded baseline scenarios (see simulator/test/baseline_scenarios.hpp) through the C API
/// and compares the stones with the results recorded before `Simulate()` replaced the mode-based API.
/// `plugin_name` is "fcv1" or "fcv1plus"; the plugin must be loaded.
void CheckLoaderBaseline(const char* plugin_name);

} // namespace digitalcurling::test
