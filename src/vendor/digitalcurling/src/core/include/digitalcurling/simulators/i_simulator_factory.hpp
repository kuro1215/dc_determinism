// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines ISimulatorFactory

#pragma once

#include <memory>
#include <string>
#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"

namespace digitalcurling::simulators {


/// @brief Factory interface for constructing an ISimulator
///
/// Instances of `ISimulator` are created by `CreateSimulator()` on a derived class.
class ISimulatorFactory : public plugins::IPluginObjectCreator, public plugins::FactoryHandle {
public:
    ISimulatorFactory() = default;
    /// @brief Copy constructor
    ISimulatorFactory(ISimulatorFactory const&) = default;
    /// @brief Copy assignment operator
    ISimulatorFactory& operator = (ISimulatorFactory const&) = default;
    virtual ~ISimulatorFactory() override = default;

    /// @brief Creates a simulator
    /// @returns Created simulator
    virtual std::unique_ptr<ISimulator> CreateSimulator() const = 0;

    /// @brief Copies this instance
    /// @returns Copied instance
    virtual std::unique_ptr<ISimulatorFactory> Clone() const = 0;

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
