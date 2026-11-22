// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines ISimulatorStorage

#pragma once

#include <memory>
#include <string>
#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"

namespace digitalcurling::simulators {


/// @brief Storage interface for saving an ISimulator's state
///
/// When state is restored from storage using `ISimulator::Load()` or `ISimulatorStorage::CreateSimulator()`,
/// the `ISimulator` fully restores its internal state as saved by `ISimulator::Save()` or `ISimulator::CreateStorage()`.
/// In other words, simulation after restoration matches simulation from the point at which the state was saved.
///
/// Storage corresponds to a particular `ISimulator` implementation, so saving or restoring requires matching IDs from `GetSimulatorId()`.
class ISimulatorStorage : public plugins::IPluginObjectCreator, public plugins::StorageHandle {
public:
    ISimulatorStorage() = default;
    /// @brief Copy constructor
    ISimulatorStorage(ISimulatorStorage const&) = default;
    /// @brief Copy assignment operator
    ISimulatorStorage & operator = (ISimulatorStorage const&) = default;
    virtual ~ISimulatorStorage() override = default;

    /// @brief Creates a simulator with the same state as this storage
    /// @returns Created simulator
    virtual std::unique_ptr<ISimulator> CreateSimulator() const = 0;

    /// @brief Gets the corresponding simulator ID
    ///
    /// Each simulator type has a distinct simulator ID.
    ///
    /// @returns Simulator ID
    virtual std::string GetSimulatorId() const { return std::string(GetId()); }

    virtual plugins::TargetHandle* Create() const override {
        return CreateSimulator().release();
    }
};

} // namespace digitalcurling::simulators
