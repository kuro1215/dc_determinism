// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines ISimulator

#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <memory>
#include <string>
#include <nlohmann/json.hpp>
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/stone.hpp"
#include "digitalcurling/stone_coordinate.hpp"
#include "digitalcurling/stone_index.hpp"
#include "digitalcurling/vector2.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"

namespace digitalcurling::simulators {

class ISimulatorFactory;
class ISimulatorStorage;

/// @brief Interface for a physics simulator that reproduces stone behavior
///
/// Instances of `ISimulator` are created only by
/// `ISimulatorFactory::CreateInstance()` on a derived factory.
///
/// `ISimulatorStorage` can save and restore the simulation state.
/// Use `ISimulator::CreateStorage()` or `ISimulator::Save()` to save state,
/// and `ISimulatorStorage::CreateSimulator()` or `ISimulator::Load()` to restore it.
///
/// Every simulator can run a whole shot with `Simulate()`. Frame-by-frame
/// stepping is optional and is provided by `IStepSimulator`.
///
/// @note
/// An `ISimulator` instance is not thread-safe. Callers must serialize all calls on one instance
/// (including const member functions, which may update internal caches).
/// Distinct instances may be used concurrently from different threads.
///
/// @sa ISimulatorFactory
/// @sa IStepSimulator
class ISimulator : public plugins::IPluginObject, public plugins::SimulatorHandle {

// types ---
public:
    /// @brief Stores a stone's position, angle, velocity, and angular velocity
    struct StoneState : public Stone {
        /// @brief Velocity (m/s)
        Vector2 translational_velocity;
        /// @brief Angular velocity (rad/s)
        float angular_velocity;

        /// @brief Initializes all data to zero
        StoneState()
            : Stone()
            , translational_velocity()
            , angular_velocity(0.f) {}

        /// @brief Initializes from the supplied data
        /// @param[in] position Position (m)
        /// @param[in] angle Angle (radian)
        /// @param[in] translational_velocity Velocity (m/s)
        /// @param[in] angular_velocity Angular velocity (radian/s)
        StoneState(Vector2 position, float angle, Vector2 translational_velocity, float angular_velocity)
            : Stone(position, angle)
            , translational_velocity(translational_velocity)
            , angular_velocity(angular_velocity) {}

        /// @brief Checks whether the stone is outside the sheet
        /// @param[in] sheet_width Sheet width (m)
        /// @return `true` if the stone is inside the sheet
        bool IsValidOnSimulation(float sheet_width = coordinate::kSheetWidthMax) const
        {
            if (!coordinate::IsValidSheetWidth(sheet_width))
                sheet_width = coordinate::kSheetWidthMax;

            float const x_limit = sheet_width / 2.0f - Stone::kRadius;
            constexpr float y_limit = coordinate::kBackBoardY - Stone::kRadius;
            return std::abs(position.x) < x_limit && position.y <= y_limit && position.y >= 0.f;
        }
    };

    /// @brief Positions and velocities of all stones
    ///
    /// A stone absent from the board is represented by `std::nullopt`.
    using AllStones = std::array<std::optional<StoneState>, StoneCoordinate::kStoneMax>;

// member functions ---
protected:
    /// @brief Default constructor
    ISimulator() = default;
    /// @brief Copy constructor
    ISimulator(ISimulator const&) = default;
    /// @brief Copy assignment operator
    ISimulator& operator = (ISimulator const&) = default;

public:
    virtual ~ISimulator() override = default;

    /// @brief Sets the information for all stones
    /// @param[in] stones Information for all stones
    virtual void SetStones(AllStones const& stones) = 0;

    /// @brief Sets the state of the specified stone
    /// @param[in] index Index of the stone to set
    /// @param[in] stone Stone state (`std::nullopt` removes the stone)
    virtual void SetStone(StoneIndex index, std::optional<StoneState> const& stone) = 0;

    /// @brief Simulates until every stone is at rest
    ///
    /// After each internal advance, a stone that is not
    /// `StoneState::IsValidOnSimulation(sheet_width)` is removed from the board.
    /// When all stones are already at rest, nothing changes.
    ///
    /// @param[in] sheet_width Sheet width (m). A value for which
    ///   `coordinate::IsValidSheetWidth()` is `false` is treated as `coordinate::kSheetWidthMax`.
    virtual void Simulate(float sheet_width) = 0;

    /// @brief Gets the information for all stones
    ///
    /// @returns Information for all stones (a copy; it stays valid after later calls)
    virtual AllStones GetStones() const = 0;

    /// @brief Gets the simulator ID
    ///
    /// Each simulator type has a distinct simulator ID.
    ///
    /// @returns Simulator ID
    virtual std::string GetSimulatorId() const { return std::string(GetId()); }

    /// @brief Gets a copy of the factory
    ///
    /// The returned factory may contain more information than the `ISimulatorFactory` instance
    /// that created this `ISimulator`. For example, its seed may have been filled in
    /// even when the original `ISimulatorFactory` did not specify one.
    /// The copy carries the effective state of this simulator, so creating an `ISimulator`
    /// from it yields the same initial conditions.
    ///
    /// @returns Copy of the factory (it stays valid after later calls)
    virtual std::unique_ptr<ISimulatorFactory> GetFactory() const = 0;

    /// @brief Creates storage and saves the current state
    /// @returns New storage containing the saved simulation state
    virtual std::unique_ptr<ISimulatorStorage> CreateStorage() const = 0;

    /// @brief Saves the current state to storage
    /// @param[out] storage Storage
    virtual void Save(ISimulatorStorage & storage) const = 0;

    /// @brief Restores the state from storage
    /// @param[in] storage Storage
    virtual void Load(ISimulatorStorage const& storage) = 0;
};


/// @cond Doxygen_Suppress
// json
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ISimulator::StoneState, position, angle, translational_velocity, angular_velocity)
/// @endcond

} // namespace digitalcurling::simulators
