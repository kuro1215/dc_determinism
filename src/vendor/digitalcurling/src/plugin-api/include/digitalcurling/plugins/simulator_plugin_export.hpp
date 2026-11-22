// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines plugin export macros

#pragma once

#ifndef __EMSCRIPTEN__

#include "digitalcurling/plugins/detail/simulator_capi_exports.hpp"

#define DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN_INNER(FactoryClass, StorageClass, SimulatorClass, CalculateShotV2) \
    static_assert(std::is_base_of_v<digitalcurling::simulators::ISimulatorFactory, FactoryClass>, "FactoryClass must derive from ISimulatorFactory"); \
    static_assert(std::is_base_of_v<digitalcurling::simulators::ISimulatorStorage, StorageClass>, "StorageClass must derive from ISimulatorStorage"); \
    static_assert(std::is_base_of_v<digitalcurling::simulators::ISimulator, SimulatorClass>, "SimulatorClass must derive from ISimulator"); \
    \
    static const DigitalCurling_PluginV2SimulatorApi g_simulator_api_v2_instance = { \
        &digitalcurling::plugins::detail::V2SimulatorGetFactory<FactoryClass>, \
        &digitalcurling::plugins::detail::V2SimulatorSave<StorageClass>, \
        &digitalcurling::plugins::detail::V2SimulatorLoad<StorageClass>, \
        digitalcurling::plugins::detail::V2SimulatorStepEntry<SimulatorClass>(), \
        &digitalcurling::plugins::detail::V2SimulatorSimulate<SimulatorClass>, \
        &digitalcurling::plugins::detail::V2SimulatorSetStones<SimulatorClass>, \
        &digitalcurling::plugins::detail::V2SimulatorSetStone<SimulatorClass>, \
        digitalcurling::plugins::detail::V2SimulatorStoppedEntry<SimulatorClass>(), \
        &digitalcurling::plugins::detail::V2SimulatorGetStones<SimulatorClass>, \
        digitalcurling::plugins::detail::V2SimulatorSecondsEntry<SimulatorClass>(), \
        CalculateShotV2 \
    }; \
    DIGITALCURLING_EXPORT_PLUGIN_INNER(digitalcurling::plugins::PluginType::simulator, FactoryClass, StorageClass, SimulatorClass, nullptr, 0u, &g_simulator_api_v2_instance, static_cast<uint32_t>(sizeof(DigitalCurling_PluginV2SimulatorApi)))

/// @endcond


/// @brief Macro for exporting a simulator plugin
///
/// A `SimulatorClass` derived from `IStepSimulator` exports the step callbacks
/// (`step`, `are_all_stones_stopped`, `get_seconds_per_frame`); a class that only derives from
/// `ISimulator` exports a simulate-only plugin (those three callbacks are NULL).
///
/// @param FactoryClass Class derived from ISimulatorFactory
/// @param StorageClass Class derived from ISimulatorStorage
/// @param SimulatorClass Class derived from ISimulator (or IStepSimulator)
#define DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN(FactoryClass, StorageClass, SimulatorClass) \
    DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN_INNER(FactoryClass, StorageClass, SimulatorClass, nullptr)

/// @brief Macro for exporting a simulator plugin that supports inverse shot calculation
/// @param FactoryClass Class derived from ISimulatorFactory
/// @param StorageClass Class derived from ISimulatorStorage
/// @param SimulatorClass Class derived from ISimulator (or IStepSimulator)
/// @param CalculateShotFunction Simulator shot calculation function (`nullptr` if inverse calculation is unsupported)
#define DIGITALCURLING_EXPORT_INVERTIBLE_SIMULATOR_PLUGIN(FactoryClass, StorageClass, SimulatorClass, CalculateShotFunction) \
    static constexpr auto g_simulator_calculate_shot_v2 = &digitalcurling::plugins::detail::V2SimulatorCalculateShot<SimulatorClass, CalculateShotFunction>; \
    DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN_INNER(FactoryClass, StorageClass, SimulatorClass, g_simulator_calculate_shot_v2)

#else

#include "digitalcurling/plugins/detail/simulator_wasm_exports.hpp"

/// @brief Exports a simulator WASM module for Emscripten
/// @param FactoryClass Class derived from ISimulatorFactory
/// @param StorageClass Argument for native plugin compatibility (unused in WASM)
/// @param SimulatorClass Argument for native plugin compatibility (unused in WASM)
#define DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN(FactoryClass, StorageClass, SimulatorClass) \
    DIGITALCURLING_EXPORT_SIMULATOR_WASM(FactoryClass)

/// @brief Exports a simulator WASM module for Emscripten
/// @param FactoryClass Class derived from ISimulatorFactory
/// @param StorageClass Argument for native plugin compatibility (unused in WASM)
/// @param SimulatorClass Argument for native plugin compatibility (unused in WASM)
/// @param CalculateShotFunction Simulator's inverse shot calculation function
#define DIGITALCURLING_EXPORT_INVERTIBLE_SIMULATOR_PLUGIN(FactoryClass, StorageClass, SimulatorClass, CalculateShotFunction) \
    DIGITALCURLING_EXPORT_INVERTIBLE_SIMULATOR_WASM(FactoryClass, SimulatorClass, CalculateShotFunction)

#endif // __EMSCRIPTEN__
