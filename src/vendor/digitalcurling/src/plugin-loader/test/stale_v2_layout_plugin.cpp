// A plugin built against the v2 layout that was used before the descriptor gained the sub-table
// sizes (`get_collisions` existed and `simulate` took a mode argument). The loader has to reject it
// from the descriptor alone: every callback below aborts the process, so the test passes only when
// no callback is executed.
#include <cstdlib>

#include "digitalcurling/plugins/plugin_api_v2.h"

namespace {
[[noreturn]] void must_not_run() { std::abort(); }

void free_string(char*) { must_not_run(); }
DigitalCurling_PluginV2Status create(const char*, void**, char**) { must_not_run(); }
void destroy(void*) { must_not_run(); }
DigitalCurling_PluginV2Status create_target(void*, void**, char**) { must_not_run(); }
DigitalCurling_PluginV2Status get_state(void*, char**, char**) { must_not_run(); }
DigitalCurling_PluginV2Status set_state(void*, const char*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status get_factory(void*, void**, char**) { must_not_run(); }
DigitalCurling_PluginV2Status save_load(void*, void*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status step(void*, int32_t, float, char**) { must_not_run(); }
DigitalCurling_PluginV2Status simulate(void*, uint32_t, float, char**) { must_not_run(); }
DigitalCurling_PluginV2Status set_stones(void*, const DigitalCurling_PluginV2StoneCoordinate*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status set_stone(void*, uint32_t, const DigitalCurling_PluginV2StoneState*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status stopped(void*, DigitalCurling_PluginV2Bool*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status get_stones(void*, DigitalCurling_PluginV2StoneCoordinate*, char**) { must_not_run(); }
DigitalCurling_PluginV2Status seconds(void*, float*, char**) { must_not_run(); }

// Local copies of the pre-change definitions.
struct StaleSimulatorApi {
    DigitalCurling_PluginV2GetFactory get_factory;
    DigitalCurling_PluginV2SaveLoad save;
    DigitalCurling_PluginV2SaveLoad load;
    DigitalCurling_PluginV2Step step;
    DigitalCurling_PluginV2Status (DIGITALCURLING_PLUGIN_V2_CALL *simulate)(void*, uint32_t, float, char**);
    DigitalCurling_PluginV2SetStones set_stones;
    DigitalCurling_PluginV2SetStone set_stone;
    DigitalCurling_PluginV2AreStopped are_all_stones_stopped;
    DigitalCurling_PluginV2GetStones get_stones;
    DigitalCurling_PluginV2GetState get_collisions;
    DigitalCurling_PluginV2GetFloat get_seconds_per_frame;
    DigitalCurling_PluginV2CalculateShot calculate_shot;
};
struct StalePluginApiV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t plugin_kind;
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
    const void* player;
    const StaleSimulatorApi* simulator;
};

const StaleSimulatorApi simulator = {get_factory, save_load, save_load, step, simulate, set_stones, set_stone, stopped, get_stones, get_state, seconds, nullptr};
const StalePluginApiV2 descriptor = {
    DIGITALCURLING_PLUGIN_ABI_V2, sizeof(StalePluginApiV2), DIGITALCURLING_PLUGIN_V2_SIMULATOR, "v2-stale-layout",
    free_string, create, create, destroy, destroy, create_target, destroy, get_state, set_state, set_state, nullptr, &simulator};
}

extern "C" DIGITALCURLING_PLUGIN_V2_EXPORT const DigitalCurling_PluginApiV2* DIGITALCURLING_PLUGIN_V2_CALL dc_get_plugin_api_v2(void) {
    return reinterpret_cast<const DigitalCurling_PluginApiV2*>(&descriptor);
}
