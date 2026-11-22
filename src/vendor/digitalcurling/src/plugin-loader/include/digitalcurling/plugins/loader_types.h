// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define C ABI-compatible types for PluginLoader

#pragma once

#include <stdint.h>

#include "digitalcurling/plugins/data_object.h"

/// @addtogroup plugin_api
/// @{

/// @brief Opaque handle ID issued by the loader for a plugin object or a snapshot
/// @details 0 is never issued and is always invalid.
/// An ID is never reused within one loader module, so a stale ID is reported as not found.
/// An ID is meaningful only to the loader module that issued it and is not a security boundary.
typedef uint64_t DigitalCurling_HandleId;

/// @brief 16-byte UUID
/// @details Used only as the output of `dc_loader_creator_compute_hash` (a UUIDv8 derived from XXH3-128).
typedef struct {
    /// @brief 16 bytes of the UUID
    uint8_t bytes[16];
} DigitalCurling_Uuid;

typedef uint32_t DigitalCurling_PluginCapabilities;
enum {
    /* Bit 0 is reserved (formerly SET_STONE, which is now mandatory). It is never set. */
    /* The simulator can calculate a shot that reaches a target. */
    DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT = 1u << 1,
    /* The simulator can advance frame by frame (step, are_all_stones_stopped, get_seconds_per_frame). */
    DIGITALCURLING_PLUGIN_CAPABILITY_STEP = 1u << 2
};

/// @}
