// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define PluginSimulator, PluginStepSimulator, InvertiblePluginSimulator and InvertiblePluginStepSimulator

#pragma once

#include <memory>
#include <optional>
#include <string>
#include "digitalcurling_plugin_loader_export.h"
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/loader_types.h"
#include "digitalcurling/plugins/wrapper_base.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

namespace digitalcurling::simulators {


/// @brief Implementation shared by the plugin simulator wrappers
///
/// Do not use this class directly. Use `PluginSimulator` (simulate only) or
/// `PluginStepSimulator` (frame-by-frame stepping available).
///
/// `GetStones()` and `GetFactory()` fetch fresh values from the plugin on every call.
///
/// @tparam Interface `ISimulator` or `IStepSimulator`
template <typename Interface>
class BasicPluginSimulator : public plugins::WrapperBase<Interface> {
public:
    using typename plugins::WrapperBase<Interface>::OwnerResource;

    /// @brief Constructor
    /// @param simulator_id Simulator ID
    /// @param instance_id Instance ID
    /// @param owner_resource Reference to the resource that owns the actual instance
    BasicPluginSimulator(
        std::string simulator_id,
        digitalcurling::plugins::HandleId instance_id,
        std::weak_ptr<OwnerResource> owner_resource
    );

    virtual void SetStones(ISimulator::AllStones const& stones) override;
    virtual void SetStone(StoneIndex index, std::optional<ISimulator::StoneState> const& stone) override;
    virtual void Simulate(float sheet_width) override;

    virtual ISimulator::AllStones GetStones() const override;
    virtual std::unique_ptr<ISimulatorFactory> GetFactory() const override;

    virtual std::unique_ptr<ISimulatorStorage> CreateStorage() const override;
    virtual void Save(ISimulatorStorage & storage) const override;
    virtual void Load(ISimulatorStorage const& storage) override;
};

/// @cond Doxygen_Suppress
// The explicit instantiation definitions are in plugin_simulator.cpp. The members are exported from the loader
// library (dllexport there, dllimport for its users), so other translation units must not instantiate them.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4910) // In the loader itself, dllexport together with extern is intended.
#endif
extern template class DIGITALCURLING_LOADER_API BasicPluginSimulator<ISimulator>;
extern template class DIGITALCURLING_LOADER_API BasicPluginSimulator<IStepSimulator>;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
/// @endcond


/// @brief Physics simulator accessed through a plugin (simulate only)
///
/// Created for a plugin that does not support stepping (`DIGITALCURLING_PLUGIN_CAPABILITY_STEP` is not set).
/// It only implements `ISimulator`; it is not an `IStepSimulator`.
class PluginSimulator : public BasicPluginSimulator<ISimulator> {
public:
    using BasicPluginSimulator<ISimulator>::BasicPluginSimulator;
};

/// @brief Physics simulator accessed through a plugin (stepping available)
///
/// Created for a plugin that supports stepping (`DIGITALCURLING_PLUGIN_CAPABILITY_STEP` is set).
/// `Simulate()` runs the plugin's own `simulate`.
class DIGITALCURLING_LOADER_API PluginStepSimulator : public BasicPluginSimulator<IStepSimulator> {
public:
    using BasicPluginSimulator<IStepSimulator>::BasicPluginSimulator;

    /// @brief Advance one frame
    ///
    /// This goes through the plugin's `step` as `Step(1, coordinate::kSheetWidthMax)`.
    /// Unlike the native `Step()` of a simulator, stones that are outside the maximum
    /// sheet width are removed afterwards.
    virtual void Step() override { Step(1, coordinate::kSheetWidthMax); }

    /// @brief Advance by the specified number of frames
    /// @param[in] frames Number of frames to advance
    /// @param[in] sheet_width Sheet width (m); stones that are not valid on this sheet are removed after each frame
    virtual void Step(int frames, float sheet_width);

    virtual bool AreAllStonesStopped() const override;
    virtual float GetSecondsPerFrame() const override;
};

/// @brief Adds `CalculateShot()` to a plugin simulator wrapper
/// @tparam Base `PluginSimulator` or `PluginStepSimulator`
template <typename Base>
class InvertiblePluginSimulatorMixin : public Base {
public:
    using Base::Base;

    /// @brief Calculate a shot that passes the specified point at the specified speed
    /// @param target_position Target point
    /// @param target_speed Speed at the target point
    /// @param angular_velocity Shot angular velocity
    /// @return Calculated shot
    virtual moves::Shot CalculateShot(Vector2 const& target_position, float target_speed, float angular_velocity) const {
        return this->template ExecuteResourceFunc<moves::Shot>([&](auto resource) {
            return resource->calculate_shot.Execute(this->GetInstanceId(), target_position, target_speed, angular_velocity);
        });
    }
};

/// @brief Invertible physics simulator accessed through a plugin (simulate only)
/// @note
/// Here, "invertible" means that a shot achieving a target stone position and speed can be calculated to a certain accuracy,
/// not that the simulation can be run backwards or inverted exactly.
///
/// A plugin that can step and calculate shots (such as fcv1) is an `InvertiblePluginStepSimulator`, not an `InvertiblePluginSimulator`.
class InvertiblePluginSimulator : public InvertiblePluginSimulatorMixin<PluginSimulator> {
public:
    using InvertiblePluginSimulatorMixin<PluginSimulator>::InvertiblePluginSimulatorMixin;
};

/// @brief Invertible physics simulator accessed through a plugin (stepping available)
/// @note
/// Here, "invertible" means that a shot achieving a target stone position and speed can be calculated to a certain accuracy,
/// not that the simulation can be run backwards or inverted exactly.
class InvertiblePluginStepSimulator : public InvertiblePluginSimulatorMixin<PluginStepSimulator> {
public:
    using InvertiblePluginSimulatorMixin<PluginStepSimulator>::InvertiblePluginSimulatorMixin;
};

/// @brief Creates the wrapper that matches the capabilities of the plugin
///
/// The wrapper is a `PluginSimulator`, `PluginStepSimulator`, `InvertiblePluginSimulator` or
/// `InvertiblePluginStepSimulator`, depending on `DIGITALCURLING_PLUGIN_CAPABILITY_STEP` and
/// `DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT`.
///
/// @param[in] simulator_id Simulator ID
/// @param[in] instance_id Instance ID of the simulator
/// @param[in] resource Resource that owns the actual instance
/// @return Created wrapper
inline std::unique_ptr<ISimulator> CreatePluginSimulatorWrapper(
    std::string simulator_id,
    digitalcurling::plugins::HandleId instance_id,
    std::shared_ptr<plugins::detail::SimulatorPluginResource> const& resource)
{
    std::weak_ptr<plugins::detail::SimulatorPluginResource> owner = resource;
    if (resource->IsStepSimulator()) {
        if (resource->IsInvertibleSimulator())
            return std::make_unique<InvertiblePluginStepSimulator>(std::move(simulator_id), std::move(instance_id), std::move(owner));
        return std::make_unique<PluginStepSimulator>(std::move(simulator_id), std::move(instance_id), std::move(owner));
    }
    if (resource->IsInvertibleSimulator())
        return std::make_unique<InvertiblePluginSimulator>(std::move(simulator_id), std::move(instance_id), std::move(owner));
    return std::make_unique<PluginSimulator>(std::move(simulator_id), std::move(instance_id), std::move(owner));
}

} // namespace digitalcurling::simulators
