// Local builds: the recipe and toolchain library `prebuild` uses to build the C API loader library and
// the player workers from the sources vendored in this repository (no network; no command other than `prebuild`
// builds locally).
//
// This tool owns the source identity (tree hashes of `src/vendor/digitalcurling` and the dependencies below `src/vendor`,
// recorded in `src/vendor/manifest.json`, verified before every build), the cache decisions in `<repo>/build/<platform>/<compiler>/`,
// the receipts (`build-receipt.json`; the receipt layout is unchanged, so a cache written by an earlier
// version of the tool is accepted) and the exact cmake/compiler command
// lines (they are part of the cache identity). The toolchain part - locating the compiler and its
// environment (vswhere + vcvars64.bat, MSYS2/MinGW) and running a command inside it - lives in
// `scripts/build-local.ps1` (Windows hosts only; the Linux and wasm builds of `prebuild` run in Docker).
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kBuildReceiptSchema;  // "dc-determinism-build-receipt-v1"
extern const char* const kBuildReceiptName;    // "build-receipt.json"

struct BuiltLoader {
    fs::path path;  // <cache>/<compiler>/loader/bin/<library>
    Json receipt;
};

struct BuiltPlayer {
    std::vector<std::string> command;  // the command prefix that runs the worker (node first for wasm)
    Json receipt;
};

// `build_loader(compiler, root=..., reuse_cache=true)`: builds (or reuses) the loader with the local `compiler`
// (msvc, gcc, clang; on linux-arm64 also gcc-nofma and clang-nofma, the base compiler with
// `-ffp-contract=off`, see sim/compilers.hpp). `root` empty: the repository root. `reuse_cache` false (what
// `prebuild` and its check pass): a cached build is ignored and the loader is built from nothing.
BuiltLoader build_loader(const std::string& compiler, const fs::path& root = fs::path(), bool reuse_cache = true);

// `build_player(player, compiler, root=...)`: normal_dist or nd2; msvc, gcc, clang, their `-nofma` variants
// (linux-arm64) or wasm.
BuiltPlayer build_player(const std::string& player, const std::string& compiler, const fs::path& root = fs::path());

// --- parts exposed for the unit tests ----------------------------------------------------------------

// `host_platform()`: "windows-amd64" / "linux-amd64" / "linux-arm64" (DcDeterminismError on any other host).
std::string host_platform();
// `cache_base(root)`: `<root>/build/<platform>`.
fs::path cache_base(const fs::path& root);
// `_verified_tree(directory, expected, label)`: the tree hash, or DcDeterminismError.
std::string verified_tree(const fs::path& directory, const std::string& expected, const std::string& label);
Json loader_sources(const fs::path& root);
Json player_sources(const fs::path& root);
// `prepare_box2d(root, work)`: copy of the vendored Box2D with the production CMake patch.
fs::path prepare_box2d(const fs::path& root, const fs::path& work);
// The command-line flags of a player worker (identity of the player cache).
std::vector<std::string> player_flags(const std::string& compiler, const std::string& player);

struct BuildToolchain {
    std::string id;
    std::string cxx;
    std::string cc;
    std::string version;
    std::string target;
    std::string bin_dir;
    std::string generator;  // loader only
    std::vector<std::string> generator_args;
    std::string cmake;  // loader only
    std::string cmake_version;
    std::string node;  // wasm only
    std::string node_version;
    // The toolchain versions the probe reports (msvc: toolset directory, link.exe, Windows SDK, cmake, ninja; MinGW:
    // `ld --version`, the MSYS2 package versions, the hashes of the runtime DLLs); null when it reports none.
    Json facts;
};

// The flags that remove the time- and path-dependent bytes from a Windows build of `root`: msvc `/Brepro` for the
// compiler and the linker; gcc and clang `-ffile-prefix-map=<root>=.` for the compiler and
// `-Wl,--no-insert-timestamp` for the linker. Nothing for wasm and for non-Windows hosts (the Docker builds run at
// fixed paths). None of them changes the generated code.
struct ReproducibleFlags {
    std::vector<std::string> compile;
    std::vector<std::string> link;
};
ReproducibleFlags reproducible_flags(const std::string& compiler_id, const fs::path& root);

// (include directories, sources) of one player worker, in compile order.
std::pair<std::vector<fs::path>, std::vector<fs::path>> player_inputs(const fs::path& root, const std::string& player);

// The toolchain of `kind` (loader|player) for `compiler`, probed through the build script (or the hooks below).
BuildToolchain probe_build_toolchain(const fs::path& root, const std::string& kind, const std::string& compiler);
// Runs one command inside that toolchain's environment (through the build script or the hooks below);
// throws DcDeterminismError when it cannot be started or fails. `what` names the command in the errors.
void run_build_tool(const fs::path& root, const std::string& what, const std::string& kind, const std::string& compiler,
                    const fs::path& cwd, const std::vector<std::string>& argv);

// Replaces the build script (unit tests only): `probe` returns the JSON the script prints for
// `probe <kind> <compiler>`; `run` executes a command in the toolchain environment and throws
// DcDeterminismError like a failing tool; `cache` (when not empty) replaces `<root>/build/<platform>`.
struct BuildHooks {
    std::function<Json(const std::string& kind, const std::string& compiler)> probe;
    std::function<void(const std::string& what, const std::string& kind, const std::string& compiler,
                       const fs::path& cwd, const std::vector<std::string>& argv)>
        run;
    fs::path cache;
};
BuildHooks& build_hooks();

}  // namespace dc_determinism
