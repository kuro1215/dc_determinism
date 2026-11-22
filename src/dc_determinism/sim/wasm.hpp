// The WebAssembly simulator worker (`--compiler wasm`): the prebuilt dc_worker.js / dc_worker.wasm of
// prebuilt/worker/wasm (`dc_determinism prebuild --wasm-worker-only`, src/worker/wasm), run with node.
//
// It is the same dc_worker protocol as the native worker, with the simulator plugins linked statically
// into the module: the `--loader` argument names the `.wasm` file and no shared library is loaded. The
// bundle is verified against its manifest and against the repository's prebuilt native loaders
// (same simulator and dependency sources), so that results of the wasm worker and of the native
// loaders compare through the usual `loader.dc_prebuilt` identity.
#pragma once

#include <string>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {

extern const char* const kWasmCompiler;  // "wasm"
extern const char* const kWasmSchema;    // "dc-determinism-wasm-worker-v1"

struct WasmBundle {
    // The command prefix `[node, dc_worker.js]`; empty without `check_node`.
    std::vector<std::string> command;
    // The `worker` block of a result: `{path, sha256, version, manifest_entry, runtime: {node}}`.
    Json worker;
    // The loader: `path` is dc_worker.wasm; `info` is the `loader` block of a result
    // (`compiler: "wasm"`, `platform: "wasm32-emscripten"`, `dc_prebuilt` with the simulator identity).
    ResolvedLoader loader;
};

struct WasmOptions {
    // False only with the `--worker-command` hook: nothing is started through node, so neither node nor
    // the worker version is looked at (the files and the manifest identities are still verified).
    bool check_node = true;
    fs::path root;     // repository root (empty: repo_root()); tests use a synthetic one
    std::string node;  // node executable (empty: `node` on PATH); tests use a stand-in
};

// Verifies the bundle and returns what a command needs. Errors (all exit code 2 messages):
//   wasm worker manifest not found / invalid wasm worker manifest / unsafe wasm worker path,
//   wasm worker not found / wasm worker hash mismatch, wasm module not found / wasm module hash mismatch,
//   stale wasm bundle (the worker source differs from prebuilt/manifest.json or src/worker/worker.cpp),
//   wasm simulator source mismatch (against the host's prebuilt loader manifest),
//   the wasm worker requires node on PATH, wasm worker version mismatch.
WasmBundle verify_wasm_bundle(const WasmOptions& options = WasmOptions());

}  // namespace dc_determinism
