// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <nlohmann/json.hpp>

namespace digitalcurling::plugins {


/// @brief Base handle for an object that creates target objects
struct CreatorHandle { virtual ~CreatorHandle() = default; };
/// @brief Opaque handle representing a Factory object
struct FactoryHandle : public CreatorHandle { virtual ~FactoryHandle() override = default; };
/// @brief Opaque handle representing a Storage object
struct StorageHandle : public CreatorHandle { virtual ~StorageHandle() override = default; };

/// @brief Opaque handle for a plugin object
struct TargetHandle { virtual ~TargetHandle() = default; };
/// @brief Opaque handle representing a Player object
struct PlayerHandle : public TargetHandle { virtual ~PlayerHandle() override = default; };
/// @brief Opaque handle representing a Simulator object
struct SimulatorHandle : public TargetHandle { virtual ~SimulatorHandle() override = default; };


/// @brief Interface for a plugin object
class IPluginObject {
public:
    virtual ~IPluginObject() = default;

    /// @brief Gets the plugin object ID
    /// @return Plugin ID
    virtual const char* GetId() const noexcept = 0;
};

/// @brief Interface for creating a plugin object
class IPluginObjectCreator : public IPluginObject, public CreatorHandle {
public:
    virtual ~IPluginObjectCreator() override = default;

    /// @brief Creates a plugin object
    /// @return Handle of the created plugin object
    virtual TargetHandle* Create() const = 0;

    /// @brief Gets plugin object information in JSON format
    /// @return JSON object containing plugin object information
    virtual nlohmann::json ToJson() const = 0;
};

} // digitalcurling::plugins
