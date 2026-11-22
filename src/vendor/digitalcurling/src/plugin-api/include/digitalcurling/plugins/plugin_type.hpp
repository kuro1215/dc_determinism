// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines plugin types and related types

#pragma once

#include <cassert>
#include <string>
#include "digitalcurling/players/i_player.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"

namespace digitalcurling {

namespace plugins {

namespace detail {

class PlayerPluginResource;
class SimulatorPluginResource;

} // namespace digitalcurling::plugins::detail

/// @brief Enumeration of plugin types
enum class PluginType {
    /// @brief Simulator
    simulator = 0,
    /// @brief Player
    player = 1,
    /// @brief Invalid plugin type
    invalid = -1,
};


/// @cond Doxygen_Suppress
template<typename T> inline constexpr bool always_false = false;
/// @endcond

/// @brief Trait to retrieve type information corresponding to a plugin type
template <PluginType Type> struct PluginTypeTraits {
    static_assert(always_false<PluginType>, "PluginTypeTraits is not defined for this PluginType");

    /// @brief Target type
    using TargetType = void;
    /// @brief Factory type
    using FactoryType = void;
    /// @brief Storage type
    using StorageType = void;
    /// @brief Resource type
    using ResourceType = void;
    /// @brief Handle type
    using HandleType = void;
};

/// @cond Doxygen_Suppress
template <> struct PluginTypeTraits<PluginType::simulator> {
    using TargetType = digitalcurling::simulators::ISimulator;
    using FactoryType = digitalcurling::simulators::ISimulatorFactory;
    using StorageType = digitalcurling::simulators::ISimulatorStorage;
    using ResourceType = digitalcurling::plugins::detail::SimulatorPluginResource;
    using HandleType = digitalcurling::plugins::SimulatorHandle;
};
template <> struct PluginTypeTraits<PluginType::player> {
    using TargetType = digitalcurling::players::IPlayer;
    using FactoryType = digitalcurling::players::IPlayerFactory;
    using StorageType = digitalcurling::players::IPlayerStorage;
    using ResourceType = digitalcurling::plugins::detail::PlayerPluginResource;
    using HandleType = digitalcurling::plugins::PlayerHandle;
};
/// @endcond

/// @brief Trait to retrieve the PluginType corresponding to a target type
template <typename T> struct PluginTypeFor {
    static_assert(always_false<T>, "PluginTypeFor is not defined for this type");

    /// @brief Corresponding PluginType
    static constexpr PluginType value = PluginType::invalid;
};

/// @cond Doxygen_Suppress
#define REGISTER_PLUGIN_TYPE_MAPPING(TypeVal) \
    template <> struct PluginTypeFor<typename PluginTypeTraits<TypeVal>::TargetType> { static constexpr PluginType value = TypeVal; }; \
    template <> struct PluginTypeFor<typename PluginTypeTraits<TypeVal>::FactoryType> { static constexpr PluginType value = TypeVal; }; \
    template <> struct PluginTypeFor<typename PluginTypeTraits<TypeVal>::StorageType> { static constexpr PluginType value = TypeVal; };

REGISTER_PLUGIN_TYPE_MAPPING(PluginType::simulator)
REGISTER_PLUGIN_TYPE_MAPPING(PluginType::player)

// Step-capable simulators are simulators as well.
template <> struct PluginTypeFor<digitalcurling::simulators::IStepSimulator> { static constexpr PluginType value = PluginType::simulator; };

#undef REGISTER_PLUGIN_TYPE_MAPPING
/// @endcond

} // namespace digitalcurling::plugins


/// @brief Converts a PluginType to a string
/// @param type PluginType to convert
/// @param capitalize Whether to capitalize the first character
/// @return Converted string
inline std::string ToString(plugins::PluginType type, bool capitalize = false) {
    switch (type) {
        case plugins::PluginType::simulator: return capitalize ? "Simulator" : "simulator";
        case plugins::PluginType::player: return capitalize ? "Player" : "player";
        default:
            assert(false && "Invalid PluginType");
            return capitalize ? "Unknown" : "unknown";
    }
};

} // namespace digitalcurling
