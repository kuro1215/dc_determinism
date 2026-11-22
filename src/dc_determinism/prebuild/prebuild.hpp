// `dc_determinism prebuild`: rebuilds the prebuilt worker, loader and player binaries of the host it runs on and writes
// prebuilt/manifest.json. (The `dc_determinism` binary itself is built by scripts/prebuild.ps1 and scripts/prebuild.sh.)
//
// What a host builds (nothing is emulated: the Docker images must have the architecture of the host):
//  * Windows host: windows-amd64 (native: dc_worker with MSYS2 g++, loaders and players with MSVC, gcc and clang),
//    linux-amd64 (Docker: dc_worker, loaders gcc/clang, players gcc/clang) and wasm (Docker, Emscripten; see below).
//    `worker_only`: the two dc_worker binaries only; just the `source`, `binaries` and toolchain members change.
//  * Linux x86-64 host: linux-amd64 (the Docker part of the Windows host) and wasm.
//  * Linux aarch64 host: linux-arm64: dc_worker (-ffp-contract=off), loaders and players of the four compiler ids gcc,
//    clang, gcc-nofma and clang-nofma (the nofma ids add -ffp-contract=off to the C and C++ flags and must contain no
//    fused multiply-add instruction, counted with objdump in the container; each loader records its count), and wasm.
//  * Apple-silicon Mac, through `dc_determinism.sh prebuild`: the Linux binary runs in the arm64 image with `--image-id`
//    and `--build-user`: the same flow as an aarch64 Linux host, linux-arm64 only, with the in-container executor below.
//  * `wasm_worker_only` (any host): only the WebAssembly dc_worker is rebuilt and installed.
//  * `tool_fragments`: no build at all, see the end of this comment.
//
// The entries of the platforms a host does not build are carried unchanged. The Windows host accepts source changes
// (everything it builds is rewritten). Every other host refuses them (exit code 2): the worker, player and loader
// sources must equal what prebuilt/manifest.json and the loader manifests record. On the Windows host the carried
// linux-arm64 data that no longer matches the new sources (worker, players, loaders) is removed after everything else
// was installed, with a log line that names the host that rebuilds it.
//
// wasm ownership: a host builds (and `--check`s) the WebAssembly artifacts only when its Emscripten image id equals
// `toolchain.wasm.image_id` of prebuilt/manifest.json or the manifest records none. Any other host carries every wasm
// member unchanged (and says so), but only while the identities wasm depends on (worker.cpp, the player sources, the
// loader source and dependencies) are the recorded ones; otherwise it stops with exit code 2 and names the recorded
// image. An absent Emscripten image counts as another id unless `--emsdk-image` was given explicitly (then it is an
// error). A host that does not own wasm leaves `toolchain.wasm` out of the `--check` toolchain comparison.
//
// All sources come from the vendored trees of this repository (src/vendor/digitalcurling and the dependencies
// below src/vendor), verified against their recorded hashes; no network and no external Git work tree is used. Every
// manifest member that is not rebuilt (for example the `dc_determinism` section) is kept as it is.
//
// Reproducible builds: every build starts from nothing and reads a copy of the sources (src/vendor, src/worker) at ONE
// fixed absolute path (common/scratch.hpp), never the build cache and never the checkout, so that the bytes do not
// depend on where the repository is checked out; the compilers get the flags that remove timestamps and embedded paths
// (/Brepro; -ffile-prefix-map, --no-insert-timestamp; the Docker builds already run at fixed paths). The manifests
// record the toolchain: `toolchain.windows-amd64.{msvc,gcc,clang}` (compiler, linker, SDK or MSYS2 package versions,
// cmake, ninja, runtime DLL hashes), `toolchain.linux-amd64`, `toolchain.linux-arm64` (written only by an arm64 host,
// carried by the others) and `toolchain.wasm` (Docker image and id); the loader manifests record the same facts.
//
// `check` (every mode): nothing is installed. The toolchain of the platforms the host builds is compared first (a
// difference is "toolchain differs: ...", a manifest without the facts "toolchain not recorded: ...": exit code 2,
// nothing built); then everything the host builds is rebuilt and compared: one line per artifact (identical / differs,
// and for a differing PE or ELF file a second line saying whether the code sections are identical) and per manifest
// (compared member by member, the entries of the host's platforms only). Exit code 0: all identical; 1: something
// differs with the same toolchain; 2: error or toolchain. The pybind record-v2 binaries are not looked at.
//
// The order of every run is: source identity (and the refusals of a non-Windows host), Docker image checks, the wasm
// decision, builds in a scratch directory, the merged manifest in memory and its carried-member check, installation,
// manifest write and re-read check, removal of the stale linux-arm64 data (Windows host).
//
// Executors. The Linux build scripts run through `docker run` (the Docker executor) or, inside a container and with
// `--image-id ID` (hidden; the Docker CLI is not available there), directly (the in-container executor): every mount
// source is copied to its container path (a path that exists without the marker of a previous in-container run is
// refused), the copies are made writable for the build user (`--build-user UID:GID`, hidden), the script runs under
// `env -i` with the same environment the Docker executor gives it, as that user through setpriv, the `/src` and
// `/deps/*` copies are verified by tree hash afterwards and the writable mounts are copied back. The builds see the
// same paths in both executors.
//
// The hidden mode `prebuild --tool-fragment FILE [--tool-fragment FILE ...] [--check]` (no Docker, allowed in a
// container) is the one implementation of the `dc_determinism` section of the manifest (the tool itself): the scripts
// scripts/prebuild.{ps1,sh} build the binary per platform and write one fragment per platform; the freshly
// built binary then merges them into the manifest (or checks them). See `run_tool_fragments`.
//
// The Docker CLI, the host smoke runs and the toolchain (localbuild/local_build.hpp) are reached through
// hooks, so that the unit tests run the whole flow without a compiler or Docker.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kPrebuiltManifestSchema;  // "dc-determinism-prebuilt-v2"
extern const char* const kDefaultEmsdkImage;       // "emscripten/emsdk:6.0.9"
constexpr int kPrebuildSmokeSamples = 640;

struct PrebuildOptions {
    fs::path root;                              // empty: the repository root
    // Docker image of the Linux builds; empty: dc_determinism-cpu:local (an error when it is absent). Its architecture
    // must be the host's.
    std::string image;
    std::string emsdk_image = kDefaultEmsdkImage;  // the wasm players and the wasm dc_worker
    bool emsdk_image_explicit = false;             // --emsdk-image was given: a missing image is an error
    bool worker_only = false;
    // Only the WebAssembly dc_worker (prebuilt/worker/wasm/{dc_worker.js,dc_worker.wasm,manifest.json}), built in
    // `emsdk_image`; nothing else under prebuilt/ changes. Refused in a container.
    bool wasm_worker_only = false;
    // Rebuild what the host builds into the scratch directory, install nothing and report whether the installed
    // artifacts and manifests are what the sources produce (run_prebuild returns 0 identical, 1 differs, 2 toolchain).
    bool check = false;
    // Hidden, only inside a container: the id of the image the container runs and the user the builds run as
    // (UID:GID). `image_id` selects the in-container executor.
    std::string image_id;
    std::string build_user;
    // Hidden: the fragments (JSON files) of the tool binaries built by scripts/prebuild.*; the mode
    // merges them into the `dc_determinism` section of the manifest (or, with `check`, compares them with it).
    std::vector<std::string> tool_fragments;
};

struct ToolOutput {
    std::int64_t exit_code = 0;
    std::string output;  // stdout, with stderr merged in
};

// Replaces the processes of a run (unit tests only). `docker` receives the arguments after `docker`;
// `program` runs a built program (smoke runs, node) and `without_msys` removes the MSYS2 directories from
// its PATH (a statically linked binary must not need any MSYS2 DLL). A hook that throws DcDeterminismError models a
// program that cannot be started.
struct PrebuildHooks {
    std::function<ToolOutput(const std::vector<std::string>& argv, bool echo)> docker;
    std::function<ToolOutput(const std::vector<std::string>& argv, bool without_msys)> program;
    // The Docker bind-mount user (`-u uid:gid`) of a Linux host; empty on Windows hosts.
    std::function<std::string()> docker_user;
    // The machine of the Linux host (`uname -m`); the default asks the OS on POSIX and is unused on Windows.
    // When set, the host check of the Linux mode consults it on every platform.
    std::function<std::string()> host_machine;
    // Unit tests only: may change the merged manifest before its carried-member check.
    std::function<void(Json& merged)> adjust_merged_manifest;
    // A built program that reads `input` from its stdin (the host smoke run of the wasm worker); stdout only.
    std::function<ToolOutput(const std::vector<std::string>& argv, const std::string& input)> program_input;
    // The fixed scratch path of the builds (tests: a path of their own; empty: default_scratch_path()).
    fs::path scratch_path;
    // Receives each line of the report of `check` (default: standard output).
    std::function<void(const std::string& line)> report;

    // The in-container executor (unit tests replace the container and its process):
    // whether this process runs in a container (default: common/environment.hpp in_container()),
    std::function<bool()> in_container;
    // its environment as {name, value} pairs (default: the process environment) and host name (default: gethostname),
    std::function<std::vector<std::pair<std::string, std::string>>()> container_environment;
    std::function<std::string()> container_hostname;
    // whether a program is installed in it (default: found on PATH),
    std::function<bool(const std::string& program)> has_program;
    // the directory that plays "/" for the mount copies (default: "/") and
    fs::path container_root;
    // the process that runs `env -i ... bash /build.sh` (default: started for real).
    std::function<ToolOutput(const std::vector<std::string>& argv, bool echo)> container_process;
    // Whether the host is a Windows host for the tool fragments (default: is_windows()).
    std::function<bool()> windows_host;
};
PrebuildHooks& prebuild_hooks();

// --- source identity -----------------------------------------------------------------------------------

struct SourceIdentity {
    std::string worker_sha256;         // src/worker/worker.cpp (LF-normalised)
    std::string player_worker_sha256;  // src/worker/player.cpp
    Json player_library;               // {revision, git_tree, tree_sha256_lf}
    Json dependencies;                 // {fpm: {commit, git_tree, include_tree_sha256_lf}, nlohmann_json: {...}}
};

// The identity of every source, with the vendored trees verified (DcDeterminismError otherwise).
SourceIdentity source_identity(const fs::path& root);
// `players.source` of the manifest.
Json players_source_json(const SourceIdentity& identity);
// The first difference between `identity` and `manifest` (the recorded source identities), or nullopt.
// `players`: also compare the player sources.
std::optional<std::string> identity_mismatch(const Json& manifest, const SourceIdentity& identity, bool players);
// The two halves of `identity_mismatch`: the worker source alone, and the player sources alone.
std::optional<std::string> worker_mismatch(const Json& manifest, const SourceIdentity& identity);
std::optional<std::string> players_mismatch(const Json& manifest, const SourceIdentity& identity);
// The first difference between the loader identity of the sources (`source` and `dependencies` as the loader manifests
// record them) and the installed loader manifests prebuilt/loaders/<platform>/manifest.json (those that exist).
std::optional<std::string> loaders_mismatch(const fs::path& root);

// --- flags and build scripts -----------------------------------------------------------------------------

std::vector<std::string> windows_worker_flags();
std::vector<std::string> linux_worker_flags();
// The linux-arm64 dc_worker: linux_worker_flags() with -ffp-contract=off after -O2.
std::vector<std::string> linux_arm64_worker_flags();
// `target`: windows-msvc, windows-gcc, windows-clang, linux-gcc, linux-clang, linux-gcc-nofma, linux-clang-nofma or wasm.
std::vector<std::string> prebuilt_player_flags(const std::string& target, const std::string& player);

// The scripts run in the containers (LF line endings, run with `bash`). `arm64`: the linux-arm64 variants.
std::string linux_worker_script(bool arm64 = false);
std::string linux_loader_script(bool arm64 = false);
std::string linux_players_script(bool arm64 = false);
std::string wasm_players_script();

// --- the WebAssembly dc_worker -------------------------------------------------------------------------------

// The sources the WebAssembly dc_worker is built from, verified against their recorded hashes.
struct WasmSources {
    std::string worker_sha256;   // src/worker/worker.cpp (LF-normalised)
    std::string simulator_tree;  // tree hash of src/vendor/digitalcurling
    Json loader_source;          // `source` of the native loader manifests
    Json loader_dependencies;    // `dependencies` of the native loader manifests
    Json verified_dependencies;  // {name: {pin, git_tree, upstream_archive_sha256, tree_sha256_lf}}
};
// Verifies the vendored simulator and loader dependency trees against the native loader manifests and
// src/vendor/manifest.json (DcDeterminismError otherwise). `require_current_manifest`: prebuilt/manifest.json must record
// the current src/worker/worker.cpp (the standalone rebuild; a full rebuild writes that record itself).
// `loader`: the {source, dependencies} identity of the loaders just built (a full rebuild or the check); the installed
// loader manifests are then not read.
WasmSources wasm_sources(const fs::path& root, bool require_current_manifest,
                         const std::optional<std::pair<Json, Json>>& loader = std::nullopt);

// What a build adds to the sources to make the manifest of the bundle.
struct WasmWorkerFacts {
    std::string js_sha256;
    std::string wasm_sha256;
    Json build_info;                          // build-info.json written by the container script
    std::string image;                        // the Emscripten image as named, and its id and repo digests
    std::string image_id;
    std::vector<std::string> repo_digests;
    std::string host_node;                    // `node --version` of the host
    Json host_seconds_per_frame;              // {fcv1, fcv1plus}
    Json host_shots;                          // {fcv1: [...], fcv1plus: [...]}
};
// prebuilt/worker/wasm/manifest.json (schema kWasmSchema, sim/wasm.hpp); write it with manifest_text().
Json wasm_worker_manifest(const WasmSources& sources, const WasmWorkerFacts& facts);

// --- manifest -----------------------------------------------------------------------------------------------

Json worker_source_json(const std::string& sha256);
Json worker_binary_json(const std::string& path, const std::string& sha256, const std::string& compiler,
                        const std::vector<std::string>& flags, const std::string& image = std::string(),
                        const std::string& image_id = std::string());
// `extra` members are appended after `flags`.
Json player_entry_json(const std::string& path, const std::string& sha256, const std::string& platform,
                       const std::string& compiler_id, const std::string& compiler, const std::string& player,
                       const std::vector<std::string>& flags, const std::vector<std::pair<std::string, Json>>& extra);

// What one run changes in prebuilt/manifest.json; every member that is not named here is kept.
struct ManifestUpdate {
    Json source;                                 // `source` (null: kept)
    Json binaries = Json::object();              // platform -> entry: written in place, new ones added
    Json players_source;                         // `players.source` (null: kept)
    // The `players.binaries` entries whose key starts with one of these prefixes are replaced by `player_entries`
    // (also entries of the prefixes that `player_entries` does not name any more: a prefix without entries removes).
    std::vector<std::string> replaced_player_prefixes;
    Json player_entries = Json::object();
    Json toolchain = Json::object();             // members written: windows-amd64 compiler by compiler, the others whole
    std::vector<std::string> removed_binaries;   // keys of `binaries` that are removed
    std::vector<std::string> removed_toolchain;  // members of `toolchain` that are removed
};
// The manifest after the update. Inside `binaries` and `players.binaries` the entries are ordered by platform:
// windows-amd64, linux-amd64, linux-arm64, wasm (the order inside a platform is kept; new entries come last in it).
// `existing` may be an empty object (the members are then written in the order schema, source, binaries, players).
Json merge_manifest(const Json& existing, const ManifestUpdate& update);

// The manifest text: `json.dumps(indent=2)` plus a newline.
std::string manifest_text(const Json& manifest);

// --- smoke outputs ------------------------------------------------------------------------------------------------

struct SmokeRun {
    std::string label;
    fs::path path;
};
struct SmokeReport {
    std::vector<std::string> lines;
    bool failed = false;
};
// Decodes every output (PDET) of every player, and compares the others with the first one. A run that
// cannot be decoded or restores incorrectly sets `failed`.
SmokeReport check_smoke(const std::vector<std::pair<std::string, std::vector<SmokeRun>>>& players);

// --- the commands --------------------------------------------------------------------------------------------------

// The option rules (`wasm_worker_only` excludes `worker_only` and `image` and is refused in a container; `image_id` and
// `build_user` are accepted only inside a container, where `image_id` is required; the tool fragments exclude every other
// mode option).
void check_prebuild_options(const PrebuildOptions& options);

// Each returns the exit code: 0, or with `check` 0 identical / 1 differs / 2 the toolchain is not the recorded one.
int run_windows_prebuild(const PrebuildOptions& options);
// The Linux host of either architecture (the host machine decides: x86_64 builds linux-amd64, aarch64 linux-arm64).
int run_linux_prebuild(const PrebuildOptions& options);
// Only the WebAssembly dc_worker, on either host.
int run_wasm_worker_prebuild(const PrebuildOptions& options);
// The `dc_determinism` section of the manifest from the fragments of options.tool_fragments, one per platform:
//   {"platform": "linux-amd64", "path": "prebuilt/dc_determinism/linux-amd64/dc_determinism", "sha256": "...",
//    "compiler": "...", "flags": [...], "link_flags": [...], "image": "...", "image_id": "...",
//    "toolchain": {...}, "built": "<path relative to the repository root of the freshly built binary>"}
// (`image` and `image_id` only for Linux; the members up to `image_id` become the binaries entry, `toolchain` the
// toolchain member of the platform; `built`, optional, is the file that is installed and compared).
// The source identity is src/dc_determinism with src/vendor/nlohmann-json and src/vendor/miniz, verified against
// src/vendor/manifest.json. Merge: the toolchain of the fragments is compared with the recorded one first (check only),
// the entries of the platforms that have no fragment are kept when the recorded source equals the current one; when it
// differs the Windows host drops them (and removes their files), every other host stops with exit code 2.
// Check: a line per binary and per manifest member, exit code 0 / 1 / 2 as for prebuild.
int run_tool_fragments(const PrebuildOptions& options);
// The mode of the options and the host (`tool_fragments`; `wasm_worker_only`; Windows: the Windows host; Linux: the Linux
// host of the machine). Returns the exit code of the command.
int run_prebuild(const PrebuildOptions& options);

}  // namespace dc_determinism
