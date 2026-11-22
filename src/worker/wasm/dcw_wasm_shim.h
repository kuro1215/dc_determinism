// Forced include (-include) for the WebAssembly build of dc_worker.
//
// The plugin loader is linked statically into the module, so there is no shared library to open:
// worker.cpp's dlopen/dlsym/dlerror calls resolve to the functions of dcw_wasm_shim.cpp, which hand
// out the statically linked dc_loader_* C API. worker.cpp itself stays unchanged.
#pragma once

#include <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif

void* dcw_dlopen(const char* path, int flags);
void* dcw_dlsym(void* handle, const char* name);
char* dcw_dlerror(void);

#ifdef __cplusplus
}
#endif

#define dlopen dcw_dlopen
#define dlsym dcw_dlsym
#define dlerror dcw_dlerror
