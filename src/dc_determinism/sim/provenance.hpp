// Loader and worker verification against the recorded manifests, and the
// loader identity checks `compare simulator` makes.
//
// Loaders always come from `prebuilt/loaders` (origin "prebuilt").
#pragma once

#include <string>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

const char* platform_key();  // "windows-amd64" / "linux-amd64" / "linux-arm64" (compile-time)
const char* worker_file_name();  // "dc_worker.exe" / "dc_worker"

// Directory holding `<platform>/manifest.json` and the loaders: `<repo>/prebuilt/loaders`.
fs::path loaders_root();

struct ResolvedLoader {
    fs::path path;
    Json info;  // the `loader` block of a result / `verify` record
};

// `verify_dc_loader`: a prebuilt loader checked against its manifest (no observer check).
ResolvedLoader verify_dc_loader(const std::string& compiler);

// `loader_provenance`: `{path, sha256, compiler, platform, origin: "prebuilt", dc_prebuilt}`.
ResolvedLoader loader_provenance(const std::string& compiler);

fs::path default_worker_path();
std::string worker_version(const fs::path& path, double timeout_seconds = 60.0);
void worker_self_test(const fs::path& path, double timeout_seconds = 120.0);
// The same through a command prefix (`[node, dc_worker.js]` for the wasm worker; `[path]` is the above).
std::string worker_version(const std::vector<std::string>& prefix, double timeout_seconds = 60.0);
void worker_self_test(const std::vector<std::string>& prefix, double timeout_seconds = 120.0);
// `worker_manifest(root)`: `<root>/prebuilt/manifest.json` (root empty: the repository root).
Json worker_manifest(const fs::path& root = fs::path());
// `check_executable`: DcDeterminismError "worker executable not found: <path>" / "worker is not executable".
void check_executable(const fs::path& path);
// `{path, sha256, version, manifest_entry}`.
Json verify_default_worker();

// `source_identity` / `check_source_identity` (compare simulator); a loader whose origin is not "prebuilt" is an error.
Json source_identity(const Json& loader);
Json check_source_identity(const Json& left, const Json& right);

}  // namespace dc_determinism
