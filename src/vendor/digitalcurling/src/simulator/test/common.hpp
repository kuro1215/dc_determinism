#pragma once

#include <memory>
#include <gtest/gtest.h>
#include "digitalcurling/core.hpp"
#include "digitalcurling/simulators.hpp"

namespace dc = digitalcurling;
namespace dcs = digitalcurling::simulators;

namespace digitalcurling::test {

bool EqualsSimulatorStones(dcs::ISimulator::AllStones const& stones0, dcs::ISimulator::AllStones const& stones1);

/// Converts a simulator created by a factory or storage to its step interface.
/// Throws std::logic_error (a test failure) when the simulator cannot step.
std::unique_ptr<dcs::IStepSimulator> AsStepSimulator(std::unique_ptr<dcs::ISimulator> simulator);

} // unnamed namespace
