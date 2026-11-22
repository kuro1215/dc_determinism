// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines SimulatorFCV1Factory

#pragma once

#include <memory>
#include <string_view>
#include <nlohmann/json.hpp>
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "fcv1.hpp"

namespace digitalcurling::simulators {

/// @brief Factory for the FCV1 simulator
///
/// The time step of the created simulator is fixed at 0.001 s (`SimulatorFCV1::kSecondsPerFrame`).
class SimulatorFCV1Factory : public ISimulatorFactory {
public:
    /// @brief Default constructor
    SimulatorFCV1Factory() = default;
    /// @brief Copy constructor
    SimulatorFCV1Factory(SimulatorFCV1Factory const&) = default;
    /// @brief Copy assignment operator
    SimulatorFCV1Factory & operator = (SimulatorFCV1Factory const&) = default;
    virtual ~SimulatorFCV1Factory() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual std::unique_ptr<ISimulator> CreateSimulator() const override;
    virtual std::unique_ptr<ISimulatorFactory> Clone() const override;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, SimulatorFCV1Factory const&);
void from_json(nlohmann::json const&, SimulatorFCV1Factory &);
/// @endcond

} // namespace digitalcurling::simulators
