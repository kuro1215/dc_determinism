// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines SimulatorFCV1Storage

#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#include "simulator_fcv1_factory.hpp"

namespace digitalcurling::simulators {

/// @brief Storage for the FCV1 simulator
class SimulatorFCV1Storage : public ISimulatorStorage {
public:
    /// @brief Default constructor
    SimulatorFCV1Storage() = default;
    /// @brief Copy constructor
    SimulatorFCV1Storage(SimulatorFCV1Storage const&) = default;
    /// @brief Copy assignment operator
    SimulatorFCV1Storage & operator = (SimulatorFCV1Storage const&) = default;
    /// @brief Constructor
    /// @param factory Factory used to initialize the storage
    SimulatorFCV1Storage(SimulatorFCV1Factory const& factory);
    virtual ~SimulatorFCV1Storage() override = default;

    virtual const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    virtual nlohmann::json ToJson() const override;

    virtual std::unique_ptr<ISimulator> CreateSimulator() const override;

    /// @brief Factory information for the simulator saved in this storage
    SimulatorFCV1Factory factory;
    /// @brief Positions and velocities of all stones
    ISimulator::AllStones stones;
};


/// @cond Doxygen_Suppress
// json
void to_json(nlohmann::json &, SimulatorFCV1Storage const&);
void from_json(nlohmann::json const&, SimulatorFCV1Storage &);
/// @endcond

} // namespace digitalcurling::simulators
