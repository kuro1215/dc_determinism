// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT
// Stable C-only plugin ABI. This file deliberately has no DigitalCurling C++ dependency.

#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define DIGITALCURLING_PLUGIN_V2_CALL __cdecl
#if defined(__cplusplus)
#define DIGITALCURLING_PLUGIN_V2_EXPORT extern "C" __declspec(dllexport)
#else
#define DIGITALCURLING_PLUGIN_V2_EXPORT __declspec(dllexport)
#endif
#else
#define DIGITALCURLING_PLUGIN_V2_CALL
#if defined(__cplusplus)
#define DIGITALCURLING_PLUGIN_V2_EXPORT extern "C" __attribute__((visibility("default")))
#else
#define DIGITALCURLING_PLUGIN_V2_EXPORT __attribute__((visibility("default")))
#endif
#endif

#define DIGITALCURLING_PLUGIN_ABI_V2 2u

typedef int32_t DigitalCurling_PluginV2Status;
typedef uint32_t DigitalCurling_PluginV2Bool;
typedef uint32_t DigitalCurling_PluginV2Kind;
enum { DIGITALCURLING_PLUGIN_V2_SIMULATOR = 0u, DIGITALCURLING_PLUGIN_V2_PLAYER = 1u };
enum { DIGITALCURLING_PLUGIN_V2_OK = 0, DIGITALCURLING_PLUGIN_V2_ERR_UNSUPPORTED_OPERATION = 14 };

typedef struct { float x; float y; } DigitalCurling_PluginV2Vector2;
typedef struct { DigitalCurling_PluginV2Vector2 position; float angle; DigitalCurling_PluginV2Vector2 translational_velocity; float angular_velocity; } DigitalCurling_PluginV2StoneState;
/* An entry whose fields are all zero means "no stone" (the stone is not on the sheet).
 * The origin is the throw point, so a resting stone there is not a legal state. */
typedef struct { DigitalCurling_PluginV2StoneState stones[16]; } DigitalCurling_PluginV2StoneCoordinate;
typedef struct { float translational_velocity; float angular_velocity; float release_angle; } DigitalCurling_PluginV2Shot;

typedef void (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2FreeString)(char*);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2Create)(const char*, void**, char**);
typedef void (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2Destroy)(void*);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2CreateTarget)(void*, void**, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2GetState)(void*, char**, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2SetState)(void*, const char*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2GetFactory)(void*, void**, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2SaveLoad)(void*, void*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2GetGender)(void*, int32_t*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2Play)(void*, const DigitalCurling_PluginV2Shot*, DigitalCurling_PluginV2Shot*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2Step)(void*, int32_t, float, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2Simulate)(void*, float, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2SetStones)(void*, const DigitalCurling_PluginV2StoneCoordinate*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2SetStone)(void*, uint32_t, const DigitalCurling_PluginV2StoneState*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2AreStopped)(void*, DigitalCurling_PluginV2Bool*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2GetStones)(void*, DigitalCurling_PluginV2StoneCoordinate*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2GetFloat)(void*, float*, char**);
typedef DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_PluginV2CalculateShot)(void*, const DigitalCurling_PluginV2Vector2*, float, float, DigitalCurling_PluginV2Shot*, char**);

/* The player and simulator sub-tables are frozen for ABI v2: their layout never changes.
 * Extensions are added as new trailing fields of DigitalCurling_PluginApiV2, which the host
 * reads only when `struct_size` covers them. */
typedef struct {
    DigitalCurling_PluginV2GetFactory get_factory;
    DigitalCurling_PluginV2SaveLoad save;
    DigitalCurling_PluginV2SaveLoad load;
    DigitalCurling_PluginV2GetGender get_gender;
    DigitalCurling_PluginV2Play play;
} DigitalCurling_PluginV2PlayerApi;
/* Simulator callbacks.
 * `step`, `are_all_stones_stopped` and `get_seconds_per_frame` are the optional step
 * capability: they are either all non-NULL or all NULL (a partial set is rejected by the loader).
 * `simulate(simulator, sheet_width, error)` advances the simulator until every stone is at rest
 * and removes stones that are not valid on a sheet of the given width.
 * `calculate_shot` is optional on its own. Every other callback is mandatory. */
typedef struct {
    DigitalCurling_PluginV2GetFactory get_factory;
    DigitalCurling_PluginV2SaveLoad save;
    DigitalCurling_PluginV2SaveLoad load;
    DigitalCurling_PluginV2Step step;                       /* optional (step capability) */
    DigitalCurling_PluginV2Simulate simulate;
    DigitalCurling_PluginV2SetStones set_stones;
    DigitalCurling_PluginV2SetStone set_stone;
    DigitalCurling_PluginV2AreStopped are_all_stones_stopped; /* optional (step capability) */
    DigitalCurling_PluginV2GetStones get_stones;
    DigitalCurling_PluginV2GetFloat get_seconds_per_frame;  /* optional (step capability) */
    DigitalCurling_PluginV2CalculateShot calculate_shot;    /* optional */
} DigitalCurling_PluginV2SimulatorApi;
/* `player_api_size` and `simulator_api_size` are `sizeof` of the sub-tables the plugin was built
 * with (the table of the other plugin kind is NULL and its size is 0). They make a change of a
 * sub-table layout visible in `struct_size`, so a plugin built against another v2 layout is
 * rejected before any callback is used. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t plugin_kind;
    uint32_t player_api_size;
    uint32_t simulator_api_size;
    /* NUL-terminated UTF-8 without ASCII control characters, 1 to 255 bytes.
     * The host reads at most 256 bytes and rejects the plugin if no NUL is found within them. */
    const char* plugin_name;
    DigitalCurling_PluginV2FreeString free_string;
    DigitalCurling_PluginV2Create create_factory;
    DigitalCurling_PluginV2Create create_storage;
    DigitalCurling_PluginV2Destroy destroy_factory;
    DigitalCurling_PluginV2Destroy destroy_storage;
    DigitalCurling_PluginV2CreateTarget create_target;
    DigitalCurling_PluginV2Destroy destroy_target;
    DigitalCurling_PluginV2GetState object_creator_get_state;
    DigitalCurling_PluginV2SetState factory_set_state;
    DigitalCurling_PluginV2SetState storage_set_state;
    const DigitalCurling_PluginV2PlayerApi* player;
    const DigitalCurling_PluginV2SimulatorApi* simulator;
} DigitalCurling_PluginApiV2;

typedef const DigitalCurling_PluginApiV2* (DIGITALCURLING_PLUGIN_V2_CALL *DigitalCurling_GetPluginApiV2Fn)(void);

#ifdef __cplusplus
static_assert(sizeof(DigitalCurling_PluginV2Bool) == 4);
static_assert(sizeof(DigitalCurling_PluginV2Vector2) == 8);
static_assert(sizeof(DigitalCurling_PluginV2StoneState) == 24);
static_assert(sizeof(DigitalCurling_PluginV2StoneCoordinate) == 384);
static_assert(sizeof(DigitalCurling_PluginV2PlayerApi) == 5 * sizeof(void*));
static_assert(sizeof(DigitalCurling_PluginV2SimulatorApi) == 11 * sizeof(void*));
static_assert(offsetof(DigitalCurling_PluginV2SimulatorApi, simulate) == 4 * sizeof(void*));
static_assert(offsetof(DigitalCurling_PluginV2SimulatorApi, get_seconds_per_frame) == 9 * sizeof(void*));
static_assert(offsetof(DigitalCurling_PluginV2SimulatorApi, calculate_shot) == 10 * sizeof(void*));
static_assert(offsetof(DigitalCurling_PluginApiV2, plugin_kind) == 8);
static_assert(offsetof(DigitalCurling_PluginApiV2, player_api_size) == 12);
static_assert(offsetof(DigitalCurling_PluginApiV2, simulator_api_size) == 16);
static_assert(offsetof(DigitalCurling_PluginApiV2, plugin_name) >= 20);
#endif
