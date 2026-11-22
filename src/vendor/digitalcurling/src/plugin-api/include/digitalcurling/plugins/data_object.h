// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines C-ABI compatible data

#pragma once

/// @addtogroup plugin_api
/// @{

/// @brief Enumeration of error codes (C-compatible)
typedef enum {
    // --- Success ---

    /// @brief Success
    DIGITALCURLING_OK = 0,

    // --- Warnings ---

    /// @brief Unknown warning
    DIGITALCURLING_WARN_UNKNOWN = 9,

    // --- Standard Errors ---

    /// @brief Plugin not found
    DIGITALCURLING_ERR_PLUGIN_NOT_FOUND = 10,
    /// @brief Plugin version is incompatible
    DIGITALCURLING_ERR_PLUGIN_VERSION_MISMATCH = 11,
    /// @brief Invalid plugin type
    DIGITALCURLING_ERR_INVALID_PLUGIN_TYPE = 12,
    /// @brief Instance not found
    DIGITALCURLING_ERR_INSTANCE_NOT_FOUND = 13,
    /// @brief The plugin does not support this operation
    DIGITALCURLING_ERR_UNSUPPORTED_OPERATION = 14,
    /// @brief Function not found
    DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN = 19,

    // --- Error Buffers and Memory ---

    /// @brief Buffer is nullptr
    DIGITALCURLING_ERR_BUFFER_NULLPTR = 21,
    /// @brief Invalid argument
    DIGITALCURLING_ERR_INVALID_ARGUMENT = 22,
    /// @brief Memory allocation failed
    DIGITALCURLING_ERR_MEMORY_ALLOCATION = 23,
    /// @brief Cast failed
    DIGITALCURLING_ERR_FAILED_TO_CAST = 24,
    /// @brief Buffer is too small for the data
    DIGITALCURLING_ERR_BUFFER_TOO_SMALL = 25,

    // --- Data Errors ---

    /// @brief Invalid data
    DIGITALCURLING_ERR_INVALID_DATA = 30,
    /// @brief JSON parse error
    DIGITALCURLING_ERR_JSON_PARSE = 31,
    /// @brief JSON data error
    DIGITALCURLING_ERR_JSON_DATA = 32,
    /// @brief Reserved; the loader no longer returns this code (handle IDs are opaque, an unknown ID returns DIGITALCURLING_ERR_INSTANCE_NOT_FOUND)
    DIGITALCURLING_ERR_UUID_PARSE = 33,

    // --- Other Errors ---

    /// @brief Other exception
    DIGITALCURLING_ERR_OTHER_EXCEPTION = 98,
    /// @brief Unknown error
    DIGITALCURLING_ERR_UNKNOWN = 99

} DigitalCurling_ErrorCode;

/// @brief Enumeration of plugin types (C-compatible)
typedef enum {
    /// @brief Simulator plugin
    DIGITALCURLING_PLUGIN_TYPE_SIMULATOR = 0,
    /// @brief Player plugin
    DIGITALCURLING_PLUGIN_TYPE_PLAYER = 1,
    /// @brief Unknown plugin type
    DIGITALCURLING_PLUGIN_TYPE_UNKNOWN = -1
} DigitalCurling_PluginType;

/// @brief Enumeration of genders (C-compatible)
typedef enum {
    /// @brief Male
    DIGITALCURLING_GENDER_MALE = 0,
    /// @brief Female
    DIGITALCURLING_GENDER_FEMALE = 1,
    /// @brief Unknown
    DIGITALCURLING_GENDER_UNKNOWN = -1
} DigitalCurling_Gender;


/// @brief Structure representing a 2D vector (C-compatible)
typedef struct {
    /// @brief X coordinate
    float x;
    /// @brief Y coordinate
    float y;
} DigitalCurling_Vector2;

/// @brief Structure representing a stone's position and angle (C-compatible)
typedef struct {
    /// @brief Stone position
    DigitalCurling_Vector2 position;
    /// @brief Stone angle (rad)
    float angle;
} DigitalCurling_Stone;

/// @brief Structure representing a stone's position and velocity (C-compatible)
typedef struct {
    /// @brief Stone position
    DigitalCurling_Vector2 position;
    /// @brief Stone angle (rad)
    float angle;
    /// @brief Translational velocity of the stone
    DigitalCurling_Vector2 translational_velocity;
    /// @brief Angular velocity of the stone
    float angular_velocity;
} DigitalCurling_StoneState;

/// @brief Structure representing the state of all stones (C-compatible)
/// @note An entry whose fields are all zero means "no stone" (the stone is not on the sheet).
/// The origin is the throw point, so a resting stone there is not a legal state and cannot be confused with an absent one.
typedef struct {
    /// @brief Array of stone states (up to 16)
    DigitalCurling_StoneState stones[16];
} DigitalCurling_StoneCoordinate;

/// @brief Structure representing shot information (C-compatible)
typedef struct {
    /// @brief Shot translational velocity
    float translational_velocity;
    /// @brief Shot angular velocity
    float angular_velocity;
    /// @brief Shot direction (rad)
    float release_angle;
} DigitalCurling_Shot;

/// @}
