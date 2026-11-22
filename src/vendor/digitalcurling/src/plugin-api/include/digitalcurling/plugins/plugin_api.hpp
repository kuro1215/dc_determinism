// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines the plugin API

#pragma once

#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/plugins/data_object.h"
#include "digitalcurling/plugins/plugin_api_v2.h"

/// @brief Plugin API version
/// @ingroup plugin_api
#define DIGITALCURLING_PLUGIN_API_VERSION 2

namespace digitalcurling::plugins {


/// @addtogroup plugin_api
/// @{

// --- Plugin Common Functions ---

/// @brief Function pointer type for freeing a string obtained from the API
/// @param[in] str Pointer to the string to free
typedef void (*FreeStringFunc)(char* str);

/// @brief Function pointer type for creating a Factory object
/// @param[in] json Initialization parameters as a JSON string
/// @param[out] out_handle Pointer to receive the created Factory handle
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*CreateFactoryFunc)(const char* json, FactoryHandle** out_handle, char** out_error);

/// @brief Function pointer type for creating a Storage object
/// @param[in] json Initialization parameters as a JSON string
/// @param[out] out_handle Pointer to receive the created Storage handle
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*CreateStorageFunc)(const char* json, StorageHandle** out_handle, char** out_error);

/// @brief Function pointer type for creating a Player/Simulator instance
/// @param[in] creator Factory handle
/// @param[out] out_target_handle Pointer to receive the created Player/Simulator handle
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*CreateTargetFunc)(CreatorHandle* creator, TargetHandle** out_target_handle, char** out_error);

/// @brief Function pointer type for destroying an object
/// @param[in] handle Handle of the object to destroy
typedef void (*DestroyObjectFunc)(void* handle);


// --- Creator Common Functions ---

/// @brief Function pointer type for retrieving the current Creator (Factory/Storage) state as JSON
/// @param[in] creator Creator handle
/// @param[out] out_json Pointer to receive the JSON state string (the caller must free it)
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*ObjectCreatorGetStateFunc)(CreatorHandle* creator, char** out_json, char** out_error);

/// @brief Function pointer type for setting or updating Factory state
/// @param[in] creator Factory handle
/// @param[in] json JSON string containing the updates
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*FactorySetStateFunc)(FactoryHandle* creator, const char* json, char** out_error);

/// @brief Function pointer type for setting or updating Storage state
/// @param[in] creator Storage handle
/// @param[in] json JSON string containing the updates
/// @param[out] out_error Pointer to receive an error message (the caller must free it)
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*StorageSetStateFunc)(StorageHandle* creator, const char* json, char** out_error);


// --- Player Plugin Functions Definition ---

/// @brief Function pointer type for retrieving the Factory that created a Player instance
/// @param[in] player Player handle
/// @param[out] out_factory_handle Pointer to receive the created Factory handle
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*PlayerGetFactoryFunc)(PlayerHandle* player, FactoryHandle** out_factory_handle, char** out_error);

/// @brief Function pointer type for saving the current Player state to Storage
/// @param[in] player Player handle
/// @param[in] storage_handle Handle of the destination Storage
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*PlayerSaveFunc)(PlayerHandle* player, StorageHandle* storage_handle, char** out_error);

/// @brief Function pointer type for loading Player state from Storage
/// @param[in] player Player handle
/// @param[in] storage_handle Handle of the source Storage
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*PlayerLoadFunc)(PlayerHandle* player, StorageHandle* storage_handle, char** out_error);

/// @brief Function pointer type for retrieving the Player gender
/// @param[in] player Player handle
/// @param[out] out_gender Pointer to receive the gender
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*PlayerGetGenderFunc)(PlayerHandle* player, DigitalCurling_Gender* out_gender, char** out_error);

/// @brief Function pointer type for retrieving the shot actually played by the Player
/// @param[in] player Player handle
/// @param[in] shot Pointer to the ideal shot information
/// @param[out] out_shot Pointer to receive the shot played by the Player
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*PlayerPlayFunc)(PlayerHandle* player, const DigitalCurling_Shot* shot, DigitalCurling_Shot* out_shot, char** out_error);

/// @brief API function table specific to player plugins
struct PlayerApi {
    /// @brief Function to retrieve a Factory from a Player instance
    PlayerGetFactoryFunc get_factory;
    /// @brief Function to save Player state to Storage
    PlayerSaveFunc save;
    /// @brief Function to load Player state from Storage
    PlayerLoadFunc load;

    /// @brief Function to retrieve Player gender
    PlayerGetGenderFunc get_gender;
    /// @brief Function to retrieve the shot played by the Player
    PlayerPlayFunc play;
};


// --- Simulator Plugin Functions Definition ---

/// @brief Function pointer type for retrieving the Factory that created a Simulator instance
/// @param[in] sim Simulator handle
/// @param[out] out_factory_handle Pointer to receive the created Factory handle
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorGetFactoryFunc)(SimulatorHandle* sim, FactoryHandle** out_factory_handle, char** out_error);

/// @brief Function pointer type for saving the current Simulator state to Storage
/// @param[in] sim Simulator handle
/// @param[in] storage_handle Handle of the destination Storage
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorSaveFunc)(SimulatorHandle* sim, StorageHandle* storage_handle, char** out_error);

/// @brief Function pointer type for loading Simulator state from Storage
/// @param[in] sim Simulator handle
/// @param[in] storage_handle Handle of the source Storage
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorLoadFunc)(SimulatorHandle* sim, StorageHandle* storage_handle, char** out_error);

/// @brief Function pointer type for advancing the simulation by a specified number of frames
///
/// Part of the optional step capability (see `SimulatorApi`).
///
/// @param[in] sim Simulator handle
/// @param[in] frames Number of frames to advance
/// @param[in] sheet_width Sheet width
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorStepFunc)(SimulatorHandle* sim, const int frames, const float sheet_width, char** out_error);

/// @brief Function pointer type for advancing the simulation until all stones stop
///
/// Stones that are not valid on a sheet of the given width are removed after each advance.
///
/// @param[in] sim Simulator handle
/// @param[in] sheet_width Sheet width
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorSimulateFunc)(SimulatorHandle* sim, const float sheet_width, char** out_error);

/// @brief Function pointer type for setting the stone layout
/// @param[in] sim Simulator handle
/// @param[in] stones Pointer to the stone positions and velocities to set
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorSetStonesFunc)(SimulatorHandle* sim, const DigitalCurling_StoneCoordinate* stones, char** out_error);

/// @brief Function pointer type that sets the state of the specified stone
/// @param[in] sim Simulator handle
/// @param[in] stone_index Index of the stone to set
/// @param[in] stone Position and velocity of the stone to set
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorSetStoneFunc)(SimulatorHandle* sim, const unsigned char stone_index, const DigitalCurling_StoneState* stone, char** out_error);

/// @brief Function pointer type for checking whether all stones have stopped
///
/// Part of the optional step capability (see `SimulatorApi`).
///
/// @param[in] sim Simulator handle
/// @param[out] out_stopped Receives true if the stones have stopped
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorAreAllStonesStoppedFunc)(SimulatorHandle* sim, bool* out_stopped, char** out_error);

/// @brief Function pointer type for retrieving the current stone layout
/// @param[in] sim Simulator handle
/// @param[out] out_stones Pointer to a structure receiving the stone information
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorGetStonesFunc)(SimulatorHandle* sim, DigitalCurling_StoneCoordinate* out_stones, char** out_error);

/// @brief Function pointer type for retrieving the simulator time per frame in seconds
///
/// Part of the optional step capability (see `SimulatorApi`).
///
/// @param[in] sim Simulator handle
/// @param[out] out_seconds Pointer to receive the time in seconds
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorGetSecondsPerFrameFunc)(SimulatorHandle* sim, float* out_seconds, char** out_error);

/// @brief Function pointer type for calculating a shot to reach the target position
/// @param[in] sim Simulator handle
/// @param[in] target_position Coordinates of the target position
/// @param[in] target_speed Target speed upon arrival
/// @param[in] angular_velocity Initial angular velocity
/// @param[out] out_shot Pointer to receive the calculated initial speed and angle
/// @param[out] out_error Pointer to receive an error message
/// @return Error code indicating the result
typedef DigitalCurling_ErrorCode (*SimulatorCalculateShotFunc)(SimulatorHandle* sim, const DigitalCurling_Vector2* target_position, const float target_speed, const float angular_velocity, DigitalCurling_Shot* out_shot, char** out_error);

/// @brief API function table specific to simulator plugins
///
/// `step`, `are_all_stones_stopped` and `get_seconds_per_frame` form the optional step capability:
/// either all of them are provided or none of them. `calculate_shot` is optional on its own.
struct SimulatorApi {
    /// @brief Function to retrieve a Factory from a Simulator instance
    SimulatorGetFactoryFunc get_factory;
    /// @brief Function to save Simulator state to Storage
    SimulatorSaveFunc save;
    /// @brief Function to load Simulator state from Storage
    SimulatorLoadFunc load;

    /// @brief Function to advance the simulation by a specified number of frames (step capability)
    SimulatorStepFunc step;
    /// @brief Function to advance the simulation until all stones stop
    SimulatorSimulateFunc simulate;
    /// @brief Function to set the stone layout
    SimulatorSetStonesFunc set_stones;
    /// @brief Function that sets the state of the specified stone
    SimulatorSetStoneFunc set_stone;

    /// @brief Function to check whether all stones have stopped (step capability)
    SimulatorAreAllStonesStoppedFunc are_all_stones_stopped;
    /// @brief Function to retrieve the current stone layout
    SimulatorGetStonesFunc get_stones;
    /// @brief Function to retrieve the simulator time per frame (step capability)
    SimulatorGetSecondsPerFrameFunc get_seconds_per_frame;

    /// @brief Function to calculate a shot to reach the target position
    SimulatorCalculateShotFunc calculate_shot;
};


/// @brief Basic plugin information
struct PluginInfo {
    /// @brief Plugin name
    char plugin_name[256];
    /// @brief Plugin type (Player or Simulator)
    DigitalCurling_PluginType plugin_type;
    /// @brief Plugin version
    unsigned int plugin_version;
};

/// @brief Plugin API function table
struct PluginApi {
    /// @brief String release function
    FreeStringFunc free_string;

    /// @brief Factory creation function
    CreateFactoryFunc create_factory;
    /// @brief Storage creation function
    CreateStorageFunc create_storage;
    /// @brief Factory destruction function
    DestroyObjectFunc destroy_factory;
    /// @brief Storage destruction function
    DestroyObjectFunc destroy_storage;
    /// @brief Target (Player/Simulator) creation function
    CreateTargetFunc create_target;
    /// @brief Target (Player/Simulator) destruction function
    DestroyObjectFunc destroy_target;

    /// @brief Creator (Factory/Storage) state retrieval function
    ObjectCreatorGetStateFunc object_creator_get_state;
    /// @brief Factory state setter
    FactorySetStateFunc factory_set_state;
    /// @brief Storage state setter
    StorageSetStateFunc storage_set_state;

    /// @brief Player-specific API (`nullptr` for a Simulator)
    const PlayerApi* player;
    /// @brief Simulator-specific API (`nullptr` for a Player)
    const SimulatorApi* simulator;
};


#ifdef DIGITALCURLING_BUNDLE_PLUGINS

/// @brief Function to register a static plugin
/// @param api Plugin's v2 API descriptor
void RegisterStaticPluginV2(const DigitalCurling_PluginApiV2* api);

#endif // DIGITALCURLING_BUNDLE_PLUGINS

/// @}

} // namespace digitalcurling::plugins
