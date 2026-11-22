// The dlopen/dlsym/dlerror replacement of the WebAssembly dc_worker
// (see dcw_wasm_shim.h and build-wasm-worker.sh).
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "digitalcurling/plugins/loader.h"

extern "C" {

namespace {

char g_error[512];
int g_handle;

}  // namespace

// `--loader` still names a file (the `.wasm` next to the worker): it must exist, nothing is loaded
// from it. Every successful call returns the same handle.
void* dcw_dlopen(const char* path, int) {
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        std::snprintf(g_error, sizeof g_error, "cannot open %s", path);
        return nullptr;
    }
    std::fclose(file);
    g_error[0] = '\0';
    return &g_handle;
}

#define DCW_SYMBOL(name) \
    if (std::strcmp(symbol, #name) == 0) return reinterpret_cast<void*>(&name);

// The C API names worker.cpp asks the loader for.
void* dcw_dlsym(void* handle, const char* symbol) {
    if (handle != &g_handle) return nullptr;
    DCW_SYMBOL(dc_loader_create_simulator_factory)
    DCW_SYMBOL(dc_loader_create_simulator)
    DCW_SYMBOL(dc_loader_remove_simulator_instance)
    DCW_SYMBOL(dc_loader_simulator_set_stones)
    DCW_SYMBOL(dc_loader_simulator_step)
    DCW_SYMBOL(dc_loader_simulator_get_stones)
    DCW_SYMBOL(dc_loader_simulator_are_all_stones_stopped)
    DCW_SYMBOL(dc_loader_get_last_error_message)
    DCW_SYMBOL(dc_loader_simulator_simulate)
    DCW_SYMBOL(dc_loader_simulator_get_seconds_per_frame)
    DCW_SYMBOL(dc_loader_simulator_get_capabilities)
    std::snprintf(g_error, sizeof g_error, "undefined symbol: %s", symbol);
    return nullptr;
}

#undef DCW_SYMBOL

char* dcw_dlerror(void) { return g_error[0] != '\0' ? g_error : nullptr; }

}  // extern "C"
