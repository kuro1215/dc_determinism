// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define the C ABI-compatible plugin loader API
///
/// @par Thread safety
/// Plugin and snapshot management functions may be called concurrently from multiple threads.
/// Calls on one plugin instance (player, simulator, factory or storage) are not synchronized:
/// the caller must serialize them per instance. Distinct instances may be used concurrently.
/// The last error message is kept per thread.

#pragma once

#include <stddef.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif
#include "digitalcurling_plugin_loader_export.h"
#include "digitalcurling/plugins/loader_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Error Handling ---

/// @brief Get the last error message
/// @param[out] out_buffer Buffer for the error message
/// @param[in] buffer_size Buffer size
/// @param[out] out_required_size Size required to store the message, including the terminating null character
/// @return Error code indicating the result
/// @note To get the required size, pass `nullptr` as `out_buffer`; the size is returned in `out_required_size`.
/// A buffer smaller than the required size returns DIGITALCURLING_ERR_BUFFER_TOO_SMALL.
/// The message is cleared by every other loader call that succeeds, and is set by one that fails.
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_get_last_error_message(char* out_buffer, size_t buffer_size, size_t* out_required_size);

// --- Snapshot ---

/// @brief Get data from a snapshot
/// @param[in] snapshot_id Snapshot ID
/// @param[out] out_buffer Buffer that receives the data, including the terminating null character
/// @param[in] buffer_size Buffer size (must be at least the `out_snapshot_size` returned when the snapshot was created)
/// @return Error code indicating the result; DIGITALCURLING_ERR_BUFFER_TOO_SMALL if the buffer is too small
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_snapshot_get_data(DigitalCurling_HandleId snapshot_id, char* out_buffer, size_t buffer_size);

/// @brief Destroy a snapshot
/// @param[in] snapshot_id ID of the snapshot to destroy
/// @return Error code indicating the result
/// @note Snapshots are kept in memory until destroyed and there is no cap on their number or total size.
/// The caller must destroy every snapshot it creates, including after a failed or abandoned data read.
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_destroy_snapshot(DigitalCurling_HandleId snapshot_id);

// --- Core Plugin Management ---

/// @brief Load a plugin
/// @param[in] plugin_path Plugin file path as a NUL-terminated UTF-8 string on all platforms
/// @param[out] out_plugin_type Type of the loaded plugin
/// @param[out] out_plugin_name Name of the loaded plugin
/// @param[in] out_name_buffer_size Buffer capacity, at least the plugin name length plus its terminating NUL
/// @return Error code indicating the result
/// @note On error no new plugin is registered, and both output arguments remain unchanged.
/// An insufficient name buffer returns DIGITALCURLING_ERR_BUFFER_TOO_SMALL.
/// Loading the same module again succeeds without replacing its registration.
/// A different module claiming an already-registered name returns DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN.
/// Dependencies of the plugin are resolved from the plugin's own directory and the system directories only
/// (not the current working directory or `PATH`), so they must be placed next to the plugin.
/// Restricting which directories plugins are loaded from is the responsibility of the host application.
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_load_plugin(const char* plugin_path, DigitalCurling_PluginType* out_plugin_type, char* out_plugin_name, size_t out_name_buffer_size);

/// @brief Get a snapshot of loaded plugins
/// @param[in] plugin_type Plugin type to retrieve
/// @param[out] out_snapshot_id Snapshot ID
/// @param[out] out_snapshot_size Buffer size required by `dc_loader_snapshot_get_data`, including the terminating null character
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_get_loaded_plugins_snapshot(DigitalCurling_PluginType plugin_type, DigitalCurling_HandleId* out_snapshot_id, size_t* out_snapshot_size);

/// @brief Check whether the specified plugin is loaded
///
/// @param[in] plugin_type Plugin type to check
/// @param[in] plugin_name Name of the plugin to check
/// @param[out] out_loaded `true` if loaded
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_is_plugin_loaded(DigitalCurling_PluginType plugin_type, const char* plugin_name, bool* out_loaded);

// --- Creator Common Functions ---

/// @brief Get a snapshot of the creator (Factory or Storage) state
/// @param[in] creator_id Creator ID
/// @param[out] out_snapshot_id Snapshot ID
/// @param[out] out_snapshot_size Buffer size required by `dc_loader_snapshot_get_data`, including the terminating null character
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_creator_get_state_snapshot(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_snapshot_id, size_t* out_snapshot_size);

/// @brief Calculate a hash of the creator (Factory or Storage) state
/// @param[in] creator_id Creator ID
/// @param[out] out_hash Hash value as a 16-byte UUIDv8 derived from XXH3-128 of the state JSON
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_creator_compute_hash(DigitalCurling_HandleId creator_id, DigitalCurling_Uuid* out_hash);

// --- Player Instance Management ---

/// @brief Create a player factory
/// @param[in] plugin_name Name of the plugin to use
/// @param[in] json_config JSON string containing the initial configuration
/// @param[out] out_factory_id ID of the created factory
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_player_factory(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_factory_id);

/// @brief Create a player storage
/// @param[in] plugin_name Name of the plugin to use
/// @param[in] json_config JSON string containing the initial configuration
/// @param[out] out_storage_id ID of the created storage
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_player_storage(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_storage_id);

/// @brief Create a player instance
/// @param[in] creator_id ID of the creator (Factory or Storage) used to create the instance
/// @param[out] out_player_id ID of the created player
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_player(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_player_id);

/// @brief Delete a player instance
/// @param[in] instance_id ID of the player instance to delete
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_remove_player_instance(DigitalCurling_HandleId instance_id);

// --- Player Actions ---

/// @brief Save the player state to storage
/// @param[in] player_id ID of the player to save from
/// @param[in] storage_id ID of the storage to save to
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_player_save(DigitalCurling_HandleId player_id, DigitalCurling_HandleId storage_id);

/// @brief Load the player state from storage
/// @param[in] player_id ID of the player to load into
/// @param[in] storage_id ID of the storage to load from
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_player_load(DigitalCurling_HandleId player_id, DigitalCurling_HandleId storage_id);

/// @brief Get the player's gender
/// @param[in] player_id Player ID
/// @param[out] out_gender Gender
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_player_get_gender(DigitalCurling_HandleId player_id, DigitalCurling_Gender* out_gender);

/// @brief Determine the player's shot
/// @param[in] player_id Player ID
/// @param[in] shot_info Ideal shot information
/// @param[out] out_shot Determined shot information
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_player_play(DigitalCurling_HandleId player_id, const DigitalCurling_Shot* shot_info, DigitalCurling_Shot* out_shot);

// --- Simulator Instance Management ---

/// @brief Create a simulator factory
/// @param[in] plugin_name Name of the plugin to use
/// @param[in] json_config JSON string containing the initial configuration
/// @param[out] out_factory_id ID of the created factory
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_simulator_factory(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_factory_id);

/// @brief Create a simulator storage
/// @param[in] plugin_name Name of the plugin to use
/// @param[in] json_config JSON string containing the initial configuration
/// @param[out] out_storage_id ID of the created storage
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_simulator_storage(const char* plugin_name, const char* json_config, DigitalCurling_HandleId* out_storage_id);

/// @brief Create a simulator instance
/// @param[in] creator_id ID of the creator (Factory or Storage) used to create the instance
/// @param[out] out_simulator_id ID of the created simulator
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_create_simulator(DigitalCurling_HandleId creator_id, DigitalCurling_HandleId* out_simulator_id);

/// @brief Delete a simulator instance
/// @param[in] instance_id ID of the simulator instance to delete
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_remove_simulator_instance(DigitalCurling_HandleId instance_id);

// -- Simulator Actions ---

/// @brief Save the simulator state to storage
/// @param[in] simulator_id ID of the simulator to save from
/// @param[in] storage_id ID of the storage to save to
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_save(DigitalCurling_HandleId simulator_id, DigitalCurling_HandleId storage_id);

/// @brief Load the simulator state from storage
/// @param[in] simulator_id ID of the simulator to load into
/// @param[in] storage_id ID of the storage to load from
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_load(DigitalCurling_HandleId simulator_id, DigitalCurling_HandleId storage_id);

/// @brief Set the stone positions in the simulator
/// @param[in] simulator_id Simulator ID
/// @param[in] stones Array of stone coordinates
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_set_stones(DigitalCurling_HandleId simulator_id, const DigitalCurling_StoneCoordinate* stones);

/// @brief Sets the state of the specified stone
/// @param[in] simulator_id Simulator ID
/// @param[in] stone_index Index of the stone to set
/// @param[in] stone Position and velocity of the stone to set
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_set_stone(DigitalCurling_HandleId simulator_id, const unsigned char stone_index, const DigitalCurling_StoneState* stone);

/// @brief Advance the simulation by the specified number of frames
///
/// Requires `DIGITALCURLING_PLUGIN_CAPABILITY_STEP`; otherwise `DIGITALCURLING_ERR_UNSUPPORTED_OPERATION` is returned.
///
/// @param[in] simulator_id Simulator ID
/// @param[in] frames Number of frames to advance
/// @param[in] sheet_width Sheet width
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_step(DigitalCurling_HandleId simulator_id, const int frames, const float sheet_width);

/// @brief Run the simulation until every stone is at rest
///
/// Stones that are not valid on a sheet of the given width are removed. An invalid width is treated as the maximum sheet width.
/// The function name is unchanged from earlier v4.1 builds, but its signature changed (the mode argument was removed).
///
/// @param[in] simulator_id Simulator ID
/// @param[in] sheet_width Sheet width
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_simulate(DigitalCurling_HandleId simulator_id, const float sheet_width);

/// @brief Get the current stone positions in the simulator
/// @param[in] simulator_id Simulator ID
/// @param[out] out_stones Array to store stone coordinates
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_get_stones(DigitalCurling_HandleId simulator_id, DigitalCurling_StoneCoordinate* out_stones);

/// @brief Check whether all stones have stopped
///
/// Requires `DIGITALCURLING_PLUGIN_CAPABILITY_STEP`; otherwise `DIGITALCURLING_ERR_UNSUPPORTED_OPERATION` is returned.
///
/// @param[in] simulator_id Simulator ID
/// @param[out] out_stopped `true` if stopped
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_are_all_stones_stopped(DigitalCurling_HandleId simulator_id, bool* out_stopped);

/// @brief Get the number of seconds per frame
///
/// Requires `DIGITALCURLING_PLUGIN_CAPABILITY_STEP`; otherwise `DIGITALCURLING_ERR_UNSUPPORTED_OPERATION` is returned.
///
/// @param[in] simulator_id Simulator ID
/// @param[out] out_seconds Number of seconds per frame
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_get_seconds_per_frame(DigitalCurling_HandleId simulator_id, float* out_seconds);

/// @brief Gets the optional operations supported by the simulator
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_get_capabilities(DigitalCurling_HandleId simulator_id, DigitalCurling_PluginCapabilities* out_capabilities);

/// @brief Calculate a shot to the target
/// @param[in] simulator_id Simulator ID
/// @param[in] target_position Target position
/// @param[in] target_speed Target speed
/// @param[in] angular_velocity Angular velocity
/// @param[out] out_shot Calculated shot information
/// @return Error code indicating the result
DigitalCurling_ErrorCode DIGITALCURLING_LOADER_API dc_loader_simulator_calculate_shot(
    DigitalCurling_HandleId simulator_id,
    const DigitalCurling_Vector2* target_position,
    const float target_speed,
    const float angular_velocity,
    DigitalCurling_Shot* out_shot
);

#ifdef __cplusplus
}
#endif
