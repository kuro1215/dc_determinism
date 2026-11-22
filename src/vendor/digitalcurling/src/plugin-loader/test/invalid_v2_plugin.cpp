#include "digitalcurling/plugins/plugin_api_v2.h"

extern "C" DIGITALCURLING_PLUGIN_V2_EXPORT const DigitalCurling_PluginApiV2* DIGITALCURLING_PLUGIN_V2_CALL dc_get_plugin_api_v2(void) {
    static const DigitalCurling_PluginApiV2 invalid{DIGITALCURLING_PLUGIN_ABI_V2, 0};
    return &invalid;
}
