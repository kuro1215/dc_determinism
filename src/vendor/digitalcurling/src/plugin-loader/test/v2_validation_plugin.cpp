#include <array>
#include "digitalcurling/plugins/plugin_api_v2.h"

#ifndef V2_VALIDATION_MODE
#define V2_VALIDATION_MODE 0
#endif

// Modes:
//  0 null descriptor               1 missing create_target        2 wrong ABI version
//  3 wrong struct_size             4 invalid plugin kind          5 / 6 step capable, no calculate_shot
//  7 no entry point                8 simulate only                9 simulate only + calculate_shot
// 10 step + calculate_shot        11 partial step set (step only)
// 12 partial step set (are_all_stones_stopped + get_seconds_per_frame only)
// 13 wrong simulator_api_size     14 wrong player_api_size (simulator plugin)
// 15 player plugin with a wrong player_api_size
// 16 player plugin with a non-zero simulator_api_size
// 17 plugin name without a terminating NUL   18 plugin name with invalid UTF-8
// 19 plugin name with a control character
//
// Every simulator object is an int state: 1 = created, 2 = simulate() was called, 3 = step() was called.
// get_stones reports the state as the y position of stone 0, so a test can see which callback ran.

#if V2_VALIDATION_MODE == 7
extern "C" DIGITALCURLING_PLUGIN_V2_EXPORT int DIGITALCURLING_PLUGIN_V2_CALL dc_unused_plugin_symbol(void) {
    return 0;
}
#else
namespace {
void free_string(char* value) { delete[] value; }
DigitalCurling_PluginV2Status create(const char*, void** out, char**) { *out = new int(1); return 0; }
DigitalCurling_PluginV2Status create_target(void*, void** out, char**) { return create(nullptr, out, nullptr); }
void destroy(void* value) { delete static_cast<int*>(value); }
DigitalCurling_PluginV2Status state(void*, char** out, char**) { *out = new char[1]{'\0'}; return 0; }
DigitalCurling_PluginV2Status set(void*, const char*, char**) { return 0; }
DigitalCurling_PluginV2Status factory(void*, void** out, char**) { return create(nullptr, out, nullptr); }
DigitalCurling_PluginV2Status save_load(void*, void*, char**) { return 0; }
DigitalCurling_PluginV2Status step(void* target, int32_t, float, char**) { *static_cast<int*>(target) = 3; return 0; }
DigitalCurling_PluginV2Status simulate(void* target, float, char**) { *static_cast<int*>(target) = 2; return 0; }
DigitalCurling_PluginV2Status stones(void*, const DigitalCurling_PluginV2StoneCoordinate*, char**) { return 0; }
DigitalCurling_PluginV2Status stone(void*, uint32_t, const DigitalCurling_PluginV2StoneState*, char**) { return 0; }
DigitalCurling_PluginV2Status stopped(void*, DigitalCurling_PluginV2Bool* out, char**) { *out = 1; return 0; }
DigitalCurling_PluginV2Status get_stones(void* target, DigitalCurling_PluginV2StoneCoordinate* out, char**) {
    *out = {};
    out->stones[0].position.y = static_cast<float>(*static_cast<int*>(target));
    return 0;
}
DigitalCurling_PluginV2Status seconds(void*, float* out, char**) { *out = 0.1f; return 0; }
DigitalCurling_PluginV2Status shot(void*, const DigitalCurling_PluginV2Vector2*, float, float, DigitalCurling_PluginV2Shot* out, char**) {
    *out = {1.f, 2.f, 3.f};
    return 0;
}

#if V2_VALIDATION_MODE == 8
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, nullptr, simulate, stones, stone, nullptr, get_stones, nullptr, nullptr};
constexpr const char* name = "v2-simulate-only";
#elif V2_VALIDATION_MODE == 9
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, nullptr, simulate, stones, stone, nullptr, get_stones, nullptr, shot};
constexpr const char* name = "v2-simulate-only-shot";
#elif V2_VALIDATION_MODE == 10
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, step, simulate, stones, stone, stopped, get_stones, seconds, shot};
constexpr const char* name = "v2-step-shot";
#elif V2_VALIDATION_MODE == 11
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, step, simulate, stones, stone, nullptr, get_stones, nullptr, nullptr};
constexpr const char* name = "v2-partial-step-a";
#elif V2_VALIDATION_MODE == 12
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, nullptr, simulate, stones, stone, stopped, get_stones, seconds, nullptr};
constexpr const char* name = "v2-partial-step-b";
#else
const DigitalCurling_PluginV2SimulatorApi simulator = {factory, save_load, save_load, step, simulate, stones, stone, stopped, get_stones, seconds, nullptr};
#if V2_VALIDATION_MODE == 5
constexpr const char* name = "v2-no-shot-a";
#elif V2_VALIDATION_MODE == 6
constexpr const char* name = "v2-no-shot-b";
#elif V2_VALIDATION_MODE == 17
// 300 bytes without a terminating NUL; a bounded scan must reject it without reading past the array.
constexpr auto name_storage = [] {
    std::array<char, 300> value{};
    for (auto& c : value) c = 'a';
    return value;
}();
constexpr const char* name = name_storage.data();
#elif V2_VALIDATION_MODE == 18
constexpr const char* name = "v2-invalid-\xC3\x28";
#elif V2_VALIDATION_MODE == 19
constexpr const char* name = "v2-control\x01";
#else
constexpr const char* name = "v2-invalid";
#endif
#endif
#if V2_VALIDATION_MODE == 15 || V2_VALIDATION_MODE == 16
DigitalCurling_PluginV2Status gender(void*, int32_t* out, char**) { *out = 0; return 0; }
DigitalCurling_PluginV2Status play(void*, const DigitalCurling_PluginV2Shot* in, DigitalCurling_PluginV2Shot* out, char**) { *out = *in; return 0; }
const DigitalCurling_PluginV2PlayerApi player = {factory, save_load, save_load, gender, play};
#endif
DigitalCurling_PluginApiV2 descriptor = {DIGITALCURLING_PLUGIN_ABI_V2, sizeof descriptor, DIGITALCURLING_PLUGIN_V2_SIMULATOR, 0u, sizeof simulator, name, free_string, create, create, destroy, destroy, create_target, destroy, state, set, set, nullptr, &simulator};
}

extern "C" DIGITALCURLING_PLUGIN_V2_EXPORT const DigitalCurling_PluginApiV2* DIGITALCURLING_PLUGIN_V2_CALL dc_get_plugin_api_v2(void) {
#if V2_VALIDATION_MODE == 0
    return nullptr;
#elif V2_VALIDATION_MODE == 1
    descriptor.create_target = nullptr;
#elif V2_VALIDATION_MODE == 2
    descriptor.abi_version = 3;
#elif V2_VALIDATION_MODE == 3
    descriptor.struct_size = sizeof descriptor - 1;
#elif V2_VALIDATION_MODE == 4
    descriptor.plugin_kind = 99;
#elif V2_VALIDATION_MODE == 13
    descriptor.simulator_api_size = sizeof simulator - sizeof(void*);
#elif V2_VALIDATION_MODE == 14
    descriptor.player_api_size = 1;
#elif V2_VALIDATION_MODE == 15 || V2_VALIDATION_MODE == 16
    descriptor.plugin_kind = DIGITALCURLING_PLUGIN_V2_PLAYER;
    descriptor.player = &player;
    descriptor.simulator = nullptr;
    descriptor.player_api_size = sizeof player - (V2_VALIDATION_MODE == 15 ? sizeof(void*) : 0);
    descriptor.simulator_api_size = V2_VALIDATION_MODE == 16 ? sizeof(DigitalCurling_PluginV2SimulatorApi) : 0u;
#endif
    return &descriptor;
}
#endif
