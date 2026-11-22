// `dc_determinism prebuild`: the pure parts (flags, scripts, manifest merging, source identities, the smoke check)
// and the flows of the Windows host, the x86-64 and the aarch64 Linux host and the in-container executor run against
// a synthetic repository root with fake Docker, toolchain and program hooks (no compiler, Docker or network is used).
// The carried and removed linux-arm64 data, the wasm ownership, `--check` per host and the `--tool-fragment` mode of the
// `dc_determinism` section are checked after the Windows and Linux flows.
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/platform.hpp"
#include "common/scratch.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "common/tree_hash.hpp"
#include "json/writer.hpp"
#include "localbuild/local_build.hpp"
#include "prebuild/binary_sections.hpp"
#include "prebuild/prebuild.hpp"
#include "sim/provenance.hpp"
#include "testing.hpp"
#include "tempdir.hpp"

using namespace dc_determinism;

namespace {

fs::path repo() { return fs::path(DC_DETERMINISM_REPO_ROOT_DIR); }

void put_le(std::string& out, std::uint64_t value, int size) {
    for (int i = 0; i < size; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

// A minimal PE file with an executable `.text` and a data section `.rdata`; `timestamp` is the header field.
std::string make_pe(const std::string& text, const std::string& data, std::uint32_t timestamp) {
    const std::uint64_t table = 0x40 + 24;  // no optional header
    const std::uint64_t text_at = table + 2 * 40;
    const std::uint64_t data_at = text_at + text.size();
    std::string file = "MZ";
    file.resize(0x3C, '\0');
    put_le(file, 0x40, 4);
    file += std::string("PE\0\0", 4);
    put_le(file, 0x8664, 2);
    put_le(file, 2, 2);
    put_le(file, timestamp, 4);
    put_le(file, 0, 4);
    put_le(file, 0, 4);
    put_le(file, 0, 2);
    put_le(file, 0x2022, 2);
    auto section = [&](const char* name, std::uint64_t size, std::uint64_t at, std::uint32_t flags) {
        std::string padded(name);
        padded.resize(8, '\0');
        file += padded;
        put_le(file, size, 4);
        put_le(file, at, 4);
        put_le(file, size, 4);
        put_le(file, at, 4);
        file.append(12, '\0');
        put_le(file, flags, 4);
    };
    section(".text", text.size(), text_at, 0x60000020u);
    section(".rdata", data.size(), data_at, 0x40000040u);
    file += text + data;
    return file;
}

// A minimal ELF64 file with an executable `.text`, a data section `.note` and the section name table.
std::string make_elf(const std::string& text, const std::string& note) {
    const std::string names("\0.text\0.note\0.shstrtab\0", 23);
    const std::uint64_t text_at = 64;
    const std::uint64_t note_at = text_at + text.size();
    const std::uint64_t names_at = note_at + note.size();
    const std::uint64_t headers_at = names_at + names.size();
    std::string file = "\x7f" "ELF";
    file.push_back(2);
    file.push_back(1);
    file.push_back(1);
    file.resize(0x28, '\0');
    put_le(file, headers_at, 8);
    file.resize(0x3A, '\0');
    put_le(file, 64, 2);
    put_le(file, 4, 2);
    put_le(file, 3, 2);
    file.resize(64, '\0');
    file += text + note + names;
    auto header = [&](std::uint32_t name, std::uint32_t type, std::uint64_t flags, std::uint64_t offset, std::uint64_t size) {
        put_le(file, name, 4);
        put_le(file, type, 4);
        put_le(file, flags, 8);
        put_le(file, 0, 8);
        put_le(file, offset, 8);
        put_le(file, size, 8);
        file.append(24, '\0');
    };
    file.append(64, '\0');
    header(1, 1, 6, text_at, text.size());
    header(7, 1, 2, note_at, note.size());
    header(13, 3, 0, names_at, names.size());
    return file;
}

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const std::exception& error) {
        return error.what();
    }
    return std::string();
}

void put32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

// A valid PDET0001 output (the layout of tool_fake_player.cpp).
std::string pdet(std::uint32_t player = 1, std::uint32_t n = 640, bool changed = false, bool restore_ok = true) {
    std::string data = "PDET0001";
    put32(data, player);
    put32(data, 256);
    for (std::uint32_t profile = 0; profile < 4; ++profile) {
        for (std::uint32_t seed = 0; seed < 64; ++seed) {
            put32(data, profile);
            put32(data, seed);
            put32(data, n);
            data.append(224, '\0');
            for (std::uint32_t i = 0; i < n; ++i) {
                put32(data, 1);
                put32(data, 2);
                put32(data, 3);
                put32(data, changed && profile == 0 && seed == 0 && i == 0 ? 0x3f800000u : 0u);
                put32(data, 0);
                put32(data, 0);
            }
            for (std::uint32_t checkpoint : {1u, 2u, 3u, 17u, 624u, 625u}) {
                put32(data, checkpoint);
                put32(data, 1);
                put32(data, restore_ok ? 1u : 0u);
                put32(data, 1);
            }
        }
    }
    return data;
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains(const std::vector<std::string>& items, const std::string& wanted) {
    return std::find(items.begin(), items.end(), wanted) != items.end();
}

std::vector<std::string> keys_of(const Json& object) {
    std::vector<std::string> names;
    for (auto it = object.begin(); it != object.end(); ++it) names.push_back(it.key());
    return names;
}

// --- a synthetic repository root -----------------------------------------------------------------------------

struct SyntheticRoot {
    explicit SyntheticRoot(const std::string& worker_text = "int worker_cpp;\n",
                           const std::string& player_text = "int player_worker_cpp;\n") {
        root = dir.path() / "repo";
        for (const char* relative : {"src/vendor/manifest.json", "src/vendor/digitalcurling", "src/vendor/fpm",
                                     "src/vendor/nlohmann-json", "src/vendor/miniz"}) {
            fs::path from = repo() / relative;
            fs::path to = root / relative;
            fs::create_directories(to.parent_path());
            fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        }
        write_bytes(root / "src" / "worker" / "worker.cpp", worker_text);
        write_bytes(root / "src" / "worker" / "player.cpp", player_text);
        write_bytes(root / "src" / "dc_determinism" / "main.cpp", "int main() { return 0; }\n");  // the tool's own tree
        add_wasm_sources();
        identity = source_identity(root);
        write_manifest(manifest());
    }

    // The sources of the WebAssembly dc_worker: the vendored digitalcurling tree (the simulator and the player library
    // are one tree), four synthetic loader dependency trees (the vendored nlohmann-json is shared with the players), the
    // native loader manifests that record their hashes, and the smoke input.
    void add_wasm_sources() {
        fs::path deps_path = root / "src" / "vendor" / "manifest.json";
        Json deps = read_json(deps_path);
        Json archives = Json::object();
        for (const char* name : {"fpm", "nlohmann-json", "box2d", "xxhash"}) {
            if (std::string(name) != "nlohmann-json") {
                fs::path directory = root / "src" / "vendor" / name;
                // The recipe patches Box2D's CMake minimum version: that tree needs exactly one such line (the
                // synthetic trees all get it).
                write_bytes(directory / "CMakeLists.txt",
                            std::string("cmake_minimum_required(VERSION 3.5)\n# ") + name + "\n");
                Json entry = Json::object();
                entry["pin"] = std::string("pin-") + name;
                entry["git_tree"] = std::string("tree-") + name;
                entry["upstream_archive_sha256"] = sha256_hex(name);
                entry["tree_sha256_lf"] = tree_sha256_lf(directory);
                deps["dependencies"][name] = entry;
            }
            archives[name] = deps["dependencies"][name]["upstream_archive_sha256"];
        }
        write_bytes(deps_path, manifest_text(deps));
        // What the loaders of a real run record (and what the check recomputes): the vendored simulator's identity.
        Json source = loader_sources(root)["simulator"];
        Json loader = Json::object();
        loader["schema"] = "fake-loader-manifest";
        loader["source"] = source;
        loader["dependencies"] = archives;
        for (const char* platform : {"windows-amd64", "linux-amd64"}) {
            write_bytes(root / "prebuilt" / "loaders" / platform / "manifest.json", manifest_text(loader));
        }
        write_bytes(root / "src" / "worker" / "wasm" / "smoke-input.bin", "smoke input\n");
    }

    // A manifest that matches the sources and carries a section (`dc_determinism`) the prebuild does not know.
    Json manifest() const {
        Json binaries = Json::object();
        binaries["windows-amd64"] = worker_binary_json("prebuilt/worker/windows-amd64/dc_worker.exe", "old-win", "old g++",
                                                       windows_worker_flags());
        binaries["linux-amd64"] = worker_binary_json("prebuilt/worker/linux-amd64/dc_worker", "old-linux", "old g++",
                                                     linux_worker_flags(), "old-image", "sha256:old");
        Json player_binaries = Json::object();
        for (const char* key : {"windows-amd64-msvc/normal_dist", "linux-amd64-gcc/normal_dist", "linux-amd64-clang/nd2",
                                "wasm/nd2"}) {
            Json entry = Json::object();
            entry["sha256"] = "old-" + std::string(key);
            player_binaries[key] = entry;
        }
        Json players = Json::object();
        players["source"] = players_source_json(identity);
        players["binaries"] = player_binaries;
        ManifestUpdate update;
        update.source = worker_source_json(identity.worker_sha256);
        update.binaries = binaries;
        update.players_source = players["source"];
        update.player_entries = players["binaries"];
        Json out = merge_manifest(Json::object(), update);
        out["dc_determinism"] = Json::parse(R"({"source": {"path": "src/dc_determinism"}, "binaries": {"linux-amd64": {"sha256": "keep"}}})");
        return out;
    }

    void write_manifest(const Json& value) { write_bytes(root / "prebuilt" / "manifest.json", manifest_text(value)); }

    // linux-arm64 data as an aarch64 host leaves it: the worker (after linux-amd64 in `binaries`), two players
    // (immediately before the wasm entries), each with its file, the loaders with their manifest and
    // toolchain.linux-arm64.
    void add_arm64(bool worker = true, bool players = true, bool loaders = true) {
        Json value = read();
        if (loaders) {
            Json loader = read_json(root / "prebuilt" / "loaders" / "linux-amd64" / "manifest.json");
            loader["schema"] = "dc-prebuilt-linux-arm64-v1";
            loader["image_id"] = "sha256:arm64";
            write_bytes(root / "prebuilt" / "loaders" / "linux-arm64" / "gcc" / "libdigitalcurling_plugin_loader.so", "arm64 loader");
            write_bytes(root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json", manifest_text(loader));
        }
        Json arm64_toolchain = Json::object();
        arm64_toolchain["image"] = "dc_determinism-cpu:local-arm64";
        arm64_toolchain["image_id"] = "sha256:arm64";
        Json toolchain = value.contains("toolchain") ? value["toolchain"] : Json::object();
        toolchain["linux-arm64"] = arm64_toolchain;
        if (value.contains("toolchain")) {
            value["toolchain"] = toolchain;
        } else {  // right after `players`, where a prebuild puts it
            Json reordered = Json::object();
            for (auto it = value.begin(); it != value.end(); ++it) {
                reordered[it.key()] = it.value();
                if (it.key() == "players") reordered["toolchain"] = toolchain;
            }
            value = reordered;
        }
        if (worker) {
            fs::path file = root / "prebuilt" / "worker" / "linux-arm64" / "dc_worker";
            write_bytes(file, "arm64 dc_worker");
            value["binaries"]["linux-arm64"] =
                worker_binary_json("prebuilt/worker/linux-arm64/dc_worker", sha256_file(file), "g++ arm64", linux_worker_flags(),
                                   "dc_determinism-cpu:local-arm64", "sha256:arm64");
        }
        if (players) {
            Json arm64 = Json::object();
            for (const char* key : {"linux-arm64-gcc/normal_dist", "linux-arm64-gcc-nofma/nd2"}) {
                std::string path = std::string("prebuilt/players/") + key + "/worker";
                write_bytes(root / path_from_utf8(path), std::string("arm64 player ") + key);
                std::string name = key;
                arm64[key] = player_entry_json(path, sha256_file(root / path_from_utf8(path)), "linux-arm64", "gcc", "g++ arm64",
                                               name.substr(name.find('/') + 1), {"-O3"}, {{"image", "dc_determinism-cpu:local-arm64"}});
            }
            Json binaries = Json::object();
            bool placed = false;
            for (auto it = value["players"]["binaries"].begin(); it != value["players"]["binaries"].end(); ++it) {
                if (!placed && it.key().compare(0, 5, "wasm/") == 0) {
                    for (auto a = arm64.begin(); a != arm64.end(); ++a) binaries[a.key()] = a.value();
                    placed = true;
                }
                binaries[it.key()] = it.value();
            }
            if (!placed) {
                for (auto a = arm64.begin(); a != arm64.end(); ++a) binaries[a.key()] = a.value();
            }
            value["players"]["binaries"] = binaries;
        }
        write_manifest(value);
    }

    // Every file below prebuilt/ (relative path -> sha256), the manifest included.
    std::map<std::string, std::string> snapshot() const {
        std::map<std::string, std::string> files;
        for (fs::recursive_directory_iterator it(root / "prebuilt"), end; it != end; ++it) {
            if (it->is_regular_file()) {
                files[to_posix_text(fs::relative(it->path(), root / "prebuilt"))] = sha256_file(it->path());
            }
        }
        return files;
    }
    static std::string to_posix_text(const fs::path& path) {
        std::string text = path_to_utf8(path);
        std::replace(text.begin(), text.end(), '\\', '/');
        return text;
    }

    Json read() const { return read_json(root / "prebuilt" / "manifest.json"); }
    std::string text() const { return read_file(root / "prebuilt" / "manifest.json"); }

    testing::TempDir dir;
    fs::path root;
    SourceIdentity identity;
};

// --- fake hooks ------------------------------------------------------------------------------------------------

std::vector<std::pair<std::string, std::string>> mounts_of(const std::vector<std::string>& args) {
    std::vector<std::pair<std::string, std::string>> mounts;  // container path (with mode) -> host path
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != "-v") continue;
        const std::string& spec = args[i + 1];
        std::size_t split = spec.find(":/");
        mounts.emplace_back(spec.substr(split + 1), spec.substr(0, split));
    }
    return mounts;
}

fs::path host_of(const std::vector<std::string>& args, const std::string& container_prefix) {
    for (const auto& item : mounts_of(args)) {
        if (item.first == container_prefix || item.first.compare(0, container_prefix.size() + 1, container_prefix + ":") == 0) {
            return path_from_utf8(item.second);
        }
    }
    throw DcDeterminismError("the fake docker has no mount " + container_prefix);
}

std::string env_of(const std::vector<std::string>& args, const std::string& name) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "-e" && args[i + 1].compare(0, name.size() + 1, name + "=") == 0) return args[i + 1].substr(name.size() + 1);
    }
    return std::string();
}

struct FakeWorld {
    explicit FakeWorld(const std::string& worker_sha) : worker_sha(worker_sha) {
        build_hooks().probe = [this](const std::string& kind, const std::string& compiler) {
            probed.push_back(kind + "/" + compiler);
            Json answer = Json::object();
            answer["ok"] = true;
            answer["id"] = compiler;
            answer["cxx"] = "/fake/" + compiler + "/cxx.EXE";
            answer["cc"] = "/fake/" + compiler + "/cc";
            answer["version"] = compiler == "gcc" ? "g++.EXE (fake probe) 1.0" : compiler + " fake 1.0";
            answer["target"] = "x86_64-w64-mingw32";
            answer["bin_dir"] = "/fake/" + compiler;
            Json facts = Json::object();
            facts["compiler"] = answer["version"];
            facts["ninja"] = "1.13.2";
            facts["tag"] = toolchain_tag;  // another tag: another toolchain
            if (compiler == "msvc") facts["toolset_dir"] = "14.52.37025";
            answer["facts"] = facts;
            if (kind == "loader") {
                answer["generator"] = compiler == "msvc" ? "Visual Studio 18 2026" : "Ninja";
                answer["generator_args"] = compiler == "msvc" ? Json::parse(R"(["-A", "x64"])") : Json::array();
                answer["cmake"] = "/fake/cmake";
                answer["cmake_version"] = "cmake version fake 3.31";
            } else {
                answer["generator"] = nullptr;
                answer["generator_args"] = Json::array();
            }
            return answer;
        };
        build_hooks().run = [this](const std::string& what, const std::string&, const std::string& compiler, const fs::path& cwd,
                                   const std::vector<std::string>& argv) {
            if (!argv.empty() && argv[0] == "/fake/cmake") {
                cmake_run(compiler, argv);
                return;
            }
            builds.push_back(argv);
            build_cwds.push_back(cwd);
            std::string output;
            for (std::size_t i = 0; i < argv.size(); ++i) {
                if (argv[i] == "-o" && i + 1 < argv.size()) output = argv[i + 1];
                if (argv[i].compare(0, 4, "/Fe:") == 0) output = argv[i].substr(4);
            }
            if (output.empty()) throw DcDeterminismError(what + ": the fake build has no output");
            write_bytes(path_from_utf8(output), "built " + compiler + build_salt);
        };
        prebuild_hooks().docker = [this](const std::vector<std::string>& args, bool) { return docker(args); };
        prebuild_hooks().program = [this](const std::vector<std::string>& argv, bool without_msys) {
            return program(argv, without_msys);
        };
        prebuild_hooks().program_input = [this](const std::vector<std::string>& argv, const std::string& input) {
            return program_input(argv, input);
        };
        prebuild_hooks().docker_user = [] { return std::string("1000:1000"); };
        prebuild_hooks().host_machine = [this] { return host_machine; };
        prebuild_hooks().scratch_path = scratch_dir.path() / "pb";
        // Never the real container: the flows run as on a host unless a test turns the container on.
        prebuild_hooks().in_container = [this] { return in_container; };
        prebuild_hooks().container_environment = [] {
            return std::vector<std::pair<std::string, std::string>>{
                {"PATH", "/usr/local/bin:/usr/bin"}, {"DEBIAN_FRONTEND", "noninteractive"}, {"HOME", "/root"},
                {"HOSTNAME", "wrapper-host"}, {"TERM", "xterm"}, {"DC_DETERMINISM_REPO", "/w/dc_determinism"},
                {"PYTHONDONTWRITEBYTECODE", "1"}};
        };
        prebuild_hooks().container_hostname = [] { return std::string("abc123def456"); };
        prebuild_hooks().has_program = [this](const std::string& name) { return name == "node" ? has_node : (name != "setpriv" || has_setpriv); };
        prebuild_hooks().container_root = container_dir.path();
        prebuild_hooks().container_process = [this](const std::vector<std::string>& argv, bool) { return container_process(argv); };
        prebuild_hooks().windows_host = nullptr;
    }
    ~FakeWorld() {
        prebuild_hooks().scratch_path = fs::path();
        prebuild_hooks().report = nullptr;
        build_hooks().cache = fs::path();
        build_hooks().probe = nullptr;
        build_hooks().run = nullptr;
        prebuild_hooks().docker = nullptr;
        prebuild_hooks().program = nullptr;
        prebuild_hooks().program_input = nullptr;
        prebuild_hooks().docker_user = nullptr;
        prebuild_hooks().host_machine = nullptr;
        prebuild_hooks().adjust_merged_manifest = nullptr;
        prebuild_hooks().in_container = nullptr;
        prebuild_hooks().container_environment = nullptr;
        prebuild_hooks().container_hostname = nullptr;
        prebuild_hooks().has_program = nullptr;
        prebuild_hooks().container_root = fs::path();
        prebuild_hooks().container_process = nullptr;
        prebuild_hooks().windows_host = nullptr;
    }

    // The fake cmake: a configure only records itself; the build writes the loader where the recipe looks for it.
    void cmake_run(const std::string& compiler, const std::vector<std::string>& argv) {
        cmake_calls.push_back(argv);
        if (argv.size() < 3 || argv[1] != "--build") return;
        fs::path directory = path_from_utf8(argv[2]) / "src" / "plugin-loader";
        // The names the recipe looks for: the Visual Studio layout, else the host's shared library name.
        fs::path library = compiler == "msvc"
                               ? directory / "Release" / "digitalcurling_plugin_loader.dll"
                               : directory / (is_windows() ? "libdigitalcurling_plugin_loader.dll" : "libdigitalcurling_plugin_loader.so");
        write_bytes(library, "loader " + compiler + build_salt);
    }

    ToolOutput docker(const std::vector<std::string>& args) {
        docker_calls.push_back(args);
        ToolOutput result;
        if (args.size() >= 3 && args[0] == "image" && args[1] == "inspect") {
            std::string image = args.back();
            if (!present.count(image)) {
                result.exit_code = 1;
                result.output = "Error: No such image: " + image + "\n";
            } else if (args.size() > 3 && args[3] == "{{.Architecture}}") {
                result.output = (architecture.count(image) ? architecture[image] : std::string("amd64")) + "\n";
            } else if (args.size() > 3 && args[3] == "{{json .RepoDigests}}") {
                result.output = "[\"" + image + "@sha256:digest-of-" + image + "\"]\n";
            } else if (args.size() > 3) {
                result.output = "sha256:id-of-" + image + image_id_tag + (image == "emscripten/emsdk:6.0.9" ? emsdk_id_tag : std::string()) + "\n";
            }
            return result;
        }
        if (contains(args, "chown")) return result;  // hands the scratch directory to the calling user
        if (fail_run) {
            result.exit_code = 3;
            result.output = "the container failed\n";
            return result;
        }
        if (args.back() == "/repo/src/worker/wasm/build-wasm-worker.sh") {
            wasm_worker_build(args);
            return result;
        }
        std::string script = read_file(host_of(args, "/build.sh"));
        std::vector<std::string> explicit_environment;
        std::vector<std::string> mount_names;
        for (std::size_t i = 0; i + 1 < args.size(); ++i) {
            if (args[i] == "-e") explicit_environment.push_back(args[i + 1]);
        }
        for (const auto& mount : mounts_of(args)) mount_names.push_back(sanitized_mount(mount.first));
        std::sort(mount_names.begin(), mount_names.end());
        script_environments.push_back(explicit_environment);
        script_mounts.push_back(mount_names);
        return build_script(script, [&](const std::string& name) { return env_of(args, name); }, host_of(args, "/out"));
    }

    // The mount name of a container path ("/deps/fpm/include:ro" -> "deps_fpm_include"), the way the in-container executor
    // names its markers.
    static std::string sanitized_mount(const std::string& container) {
        std::string path = container.substr(0, container.find(':'));
        path = path.substr(path.empty() || path[0] != '/' ? 0 : 1);
        std::replace(path.begin(), path.end(), '/', '_');
        return path;
    }

    // What a build container does with its script: the files the real script writes below /out.
    ToolOutput build_script(const std::string& script, const std::function<std::string(const std::string&)>& env, const fs::path& out) {
        ToolOutput result;
        scripts.push_back(script);
        const bool arm64 = script.find("linux-arm64") != std::string::npos || script.find("-ffp-contract=off") != std::string::npos;
        if (script.find("digitalcurling_plugin_loader") != std::string::npos) {
            std::string comp = env("COMP");
            if (objdump_missing && script.find("objdump") != std::string::npos) {
                result.exit_code = 1;
                result.output = "objdump was not found in the image (binutils)\n";
                return result;
            }
            fs::path directory = out / comp;
            write_bytes(directory / "libdigitalcurling_plugin_loader.so", "linux loader " + comp + build_salt);
            write_bytes(directory / "compiler.txt", comp.compare(0, 3, "gcc") == 0 ? "g++ (fake Ubuntu) 13.3.0\n" : "Ubuntu clang version fake\n");
            write_bytes(directory / "target.txt", arm64 ? "aarch64-linux-gnu\n" : "x86_64-linux-gnu\n");
            write_bytes(directory / "cmake.txt", "cmake version 3.28.3\n");
            // ldd prints where each library was mapped: another address on every run.
            std::string address = "(0x00007f" + std::to_string(100000 + ++ldd_runs) + "000)";
            write_bytes(directory / "ldd.txt", "\tlinux-vdso.so.1 " + address + "\n\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 " + address + "\n");
            if (script.find("fma.txt") != std::string::npos) {
                write_bytes(directory / "fma.txt", std::to_string(fma_counts.count(comp) ? fma_counts[comp] : (comp.size() > 6 && comp.compare(comp.size() - 6, 6, "-nofma") == 0 ? 0 : 12)) + "\n");
            }
        } else if (script.find("em++") != std::string::npos) {
            write_bytes(out / "em.txt", "emcc (fake) 6.0.9\n");
            write_bytes(out / "node.txt", "v24.0.0\n");
            for (const char* player : {"normal_dist", "nd2"}) {
                write_bytes(out / "wasm" / player / "worker.js", std::string("wasm js ") + player);
                write_bytes(out / "wasm" / player / "worker.wasm", std::string("wasm module ") + player);
                write_bytes(out / "smoke" / (std::string("wasm-container-") + player + ".bin"), pdet(player == std::string("nd2") ? 2 : 1));
            }
        } else if (script.find("/out/dc_worker") != std::string::npos) {
            write_bytes(out / "compiler.txt", "g++ (fake Ubuntu) 13.3.0\n");
            std::string worker_bytes = elf_worker ? make_elf(worker_text, "note" + build_salt)
                                                  : "linux dc_worker " + env("SOURCE_SHA") + build_salt;
            write_bytes(out / "dc_worker", worker_bytes);
            result.output = "self-test: OK\nversion: dc_worker " + (wrong_linux_version ? std::string("0") : env("SOURCE_SHA")) + "\n";
        } else {
            write_bytes(out / "gcc.txt", "g++ (fake Ubuntu) 13.3.0\n");
            write_bytes(out / "clang.txt", "Ubuntu clang version fake\n");
            const std::string platform = arm64 ? "linux-arm64" : "linux-amd64";
            std::vector<std::string> comps = {"gcc", "clang"};
            if (arm64) comps = {"gcc", "clang", "gcc-nofma", "clang-nofma"};
            for (const std::string& comp : comps) {
                for (const char* player : {"normal_dist", "nd2"}) {
                    std::string dir = platform + "-" + comp;
                    write_bytes(out / dir / player / "worker", "linux " + comp + " " + player + build_salt);
                    write_bytes(out / "smoke" / (dir + "-" + player + ".bin"),
                                pdet(player == std::string("nd2") ? 2 : 1, 640, false, !bad_restore));
                }
            }
        }
        return result;
    }

    // The in-container executor: the process that runs `env -i ... bash /build.sh` finds the copies below the container root.
    ToolOutput container_process(const std::vector<std::string>& argv) {
        container_commands.push_back(argv);
        fs::path root = container_dir.path();
        std::string script = read_file(root / "build.sh");
        std::map<std::string, std::string> env;
        std::vector<std::string> explicit_environment;
        for (std::size_t i = 2; i < argv.size() && argv[i] != "setpriv" && argv[i] != "bash"; ++i) {
            std::size_t equals = argv[i].find('=');
            env[argv[i].substr(0, equals)] = argv[i].substr(equals + 1);
            bool image = argv[i].compare(0, 5, "PATH=") == 0 || argv[i].compare(0, 4, "HOME") == 0 || argv[i].compare(0, 8, "HOSTNAME") == 0 ||
                         argv[i] == "DEBIAN_FRONTEND=noninteractive";
            if (!image) explicit_environment.push_back(argv[i]);
        }
        if (tamper_src) write_bytes(root / "src" / "tampered.txt", "changed by the script");
        // The markers say which run copied what: the mounts of this script are the ones of the newest run.
        std::vector<std::string> mount_names;
        long newest = 0;
        for (fs::directory_iterator it(root / ".dc_determinism-prebuild-copies"), end; it != end; ++it) {
            newest = std::max(newest, std::stol(read_file(it->path())));
        }
        for (fs::directory_iterator it(root / ".dc_determinism-prebuild-copies"), end; it != end; ++it) {
            if (std::stol(read_file(it->path())) == newest) mount_names.push_back(path_to_utf8(it->path().filename()));
        }
        std::sort(mount_names.begin(), mount_names.end());
        script_environments.push_back(explicit_environment);
        script_mounts.push_back(mount_names);
        return build_script(script, [&](const std::string& name) { return env.count(name) ? env[name] : std::string(); }, root / "out");
    }

    ToolOutput program(const std::vector<std::string>& argv, bool without_msys) {
        programs.push_back(argv);
        without_msys_flags.push_back(without_msys);
        ToolOutput result;
        const std::string& exe = argv[0];
        if (argv.size() == 2 && argv[1] == "--self-test") {
            result.output = "OK\n";
        } else if (argv.size() == 2 && argv[1] == "--version" && exe == "node") {
            result.output = "v26.0.0\n";
        } else if (argv.size() == 2 && argv[1] == "--version" && ends_with(exe, "g++.exe")) {
            result.output = "g++.exe (fake MSYS2) 16.2.0\nCopyright\n";
        } else if (argv.size() == 2 && argv[1] == "--version") {
            result.output = "dc_worker " + worker_sha + "\n";
        } else if (argv.size() == 3 && argv[0] == "node" && argv[2] == "--version") {
            result.output = (wasm_wrong_host_version ? std::string("dc_worker 0") : "dc_worker " + worker_sha) + "\n";
        } else if (argv.size() == 3 && argv[0] == "node" && argv[2] == "--self-test") {
            result.output = "OK\n";
        } else if (argv.size() == 7 && argv[0] == "node" && argv[6] == "--seconds-per-frame") {
            result.output = (wasm_host_spf_differs ? std::string("0.5") : seconds_per_frame(argv[5])) + "\n";
        } else if (argv.back() == "640") {
            std::string player_hint = argv[argv.size() - 2];
            bool nd2 = player_hint.find("nd2") != std::string::npos;
            write_bytes(path_from_utf8(player_hint), pdet(nd2 ? 2 : 1));
        } else {
            result.exit_code = 9;
        }
        return result;
    }

    static std::string seconds_per_frame(const std::string& engine) { return engine == "fcv1" ? "0.001" : "0.0005"; }

    // The result lines of the wasm worker for the smoke shots 0..2 (the last field is the wall time).
    static std::string shot_line(const std::string& engine, int shot) { return std::to_string(shot) + " ok " + engine; }

    // The Emscripten container of the wasm dc_worker: the bundle and the build-info.json it writes.
    void wasm_worker_build(const std::vector<std::string>& args) {
        fs::path repo_dir = host_of(args, "/repo");
        fs::path out = host_of(args, "/out");
        wasm_builds.push_back(args);
        write_bytes(out / "dc_worker.js", "wasm worker js " + wasm_content);
        write_bytes(out / "dc_worker.wasm", "wasm worker module " + wasm_content);
        Json info = Json::object();
        info["schema"] = wasm_info_schema;
        Json toolchain = Json::object();
        toolchain["emxx"] = "emcc (fake) 6.0.9";
        toolchain["emxx_version_text"] = "emcc (fake) 6.0.9\nCopyright";
        toolchain["node"] = "v24.0.0";
        toolchain["cmake"] = "cmake version 3.31.6";
        info["toolchain"] = toolchain;
        info["worker_source_sha256_lf"] = wasm_other_source ? std::string(64, '0') : worker_sha;
        info["override_sha256"] = std::string(64, '1');
        Json shims = Json::object();
        shims["dcw_wasm_shim.cpp"] = std::string(64, '2');
        shims["dcw_wasm_shim.h"] = std::string(64, '3');
        info["shim_sha256"] = shims;
        info["smoke_input_sha256"] =
            wasm_other_smoke_input ? std::string(64, '4') : sha256_file(repo_dir / "src" / "worker" / "wasm" / "smoke-input.bin");
        Json flags = Json::object();
        flags["configure"] = Json::parse(R"(["-S", "<repo>/src/vendor/digitalcurling", "-B", "<build>/cmake"])");
        flags["link"] = Json::parse(R"(["-O3", "-include", "<repo>/src/worker/wasm/dcw_wasm_shim.h"])");
        flags["cmake_cache"] = Json::parse(R"(["CMAKE_BUILD_TYPE:STRING=Release"])");
        flags["fp_contract"] = "compiler default";
        info["flags"] = flags;
        Json smoke = Json::object();
        smoke["version"] = wasm_wrong_container_version ? std::string("dc_worker 0") : "dc_worker " + worker_sha;
        smoke["self_test"] = "OK";
        Json spf = Json::object();
        Json shots = Json::object();
        for (const char* engine : {"fcv1", "fcv1plus"}) {
            spf[engine] = seconds_per_frame(engine);
            Json lines = Json::array();
            for (int shot = 0; shot < 3; ++shot) lines.push_back(shot_line(engine, shot));
            shots[engine] = lines;
        }
        smoke["seconds_per_frame"] = spf;
        smoke["shots"] = shots;
        info["smoke"] = smoke;
        write_bytes(out / "build-info.json", manifest_text(info));
    }

    // The host node run of the smoke shots: one result line per shot id on stdin.
    ToolOutput program_input(const std::vector<std::string>& argv, const std::string& input) {
        program_inputs.push_back(argv);
        smoke_inputs.push_back(input);
        ToolOutput result;
        std::string engine = argv.size() > 5 ? argv[5] : std::string();
        for (int shot = 0; shot < 3; ++shot) {
            result.output += shot_line(engine, shot) + (wasm_host_shots_differ && shot == 1 ? "-other" : "") + " 1.5\n";
        }
        return result;
    }

    testing::TempDir scratch_dir;  // the fixed scratch path of the builds is below it
    std::string worker_sha;
    std::string build_salt;      // appended to every fake build: another salt, other bytes
    bool elf_worker = false;     // the Linux dc_worker is a minimal ELF file: the salt changes its note, not its code
    std::string worker_text = "code";  // the code of that ELF file
    std::string toolchain_tag = "1";
    std::string image_id_tag;    // appended to the id of every Docker image
    std::string emsdk_id_tag;    // appended to the id of the Emscripten image only
    bool tamper_src = false;     // the in-container build script changes its read-only /src copy
    std::set<std::string> present = {"dc_determinism-cpu:local", "other-image:1", "emscripten/emsdk:6.0.9"};
    bool fail_run = false;
    bool wrong_linux_version = false;
    bool in_container = false;       // the prebuild runs in a container (the in-container executor)
    bool has_setpriv = true;         // setpriv is installed in that container
    bool has_node = true;            // node is on PATH (the wasm smoke test)
    bool objdump_missing = false;    // the arm64 loader script finds no objdump
    std::map<std::string, int> fma_counts;  // loader id -> fused multiply-add count (default: 12, nofma: 0)
    testing::TempDir container_dir;  // plays "/" of the container
    std::vector<std::vector<std::string>> script_environments;  // per build script: the variables passed to it
    std::vector<std::vector<std::string>> script_mounts;        // per build script: its mount names, sorted
    std::vector<std::vector<std::string>> container_commands;   // the command lines of the in-container executor
    bool bad_restore = false;
    int ldd_runs = 0;
    std::string wasm_content = "v1";  // what the wasm container builds (another value: another bundle)
    bool wasm_wrong_container_version = false;
    std::string wasm_info_schema = "dc-determinism-wasm-build-info-v1";  // what build-info.json declares
    bool wasm_wrong_host_version = false;
    bool wasm_host_spf_differs = false;
    bool wasm_host_shots_differ = false;
    bool wasm_other_source = false;
    bool wasm_other_smoke_input = false;
    std::vector<std::vector<std::string>> wasm_builds;
    std::vector<std::vector<std::string>> program_inputs;
    std::vector<std::string> smoke_inputs;
    std::string host_machine = "x86_64";                 // the Linux-host check consults it on every platform
    std::map<std::string, std::string> architecture;     // image -> Architecture (default amd64)
    std::vector<std::string> probed;
    std::vector<std::vector<std::string>> builds;
    std::vector<std::vector<std::string>> cmake_calls;  // configure and build of the loaders
    std::vector<fs::path> build_cwds;
    std::vector<std::vector<std::string>> docker_calls;
    std::vector<std::string> scripts;
    std::vector<std::vector<std::string>> programs;
    std::vector<bool> without_msys_flags;
};

}  // namespace

// --- pure parts ---------------------------------------------------------------------------------------------------

TEST(prebuild_is_a_command_of_the_tool) {
    CHECK(cli::find_handler("prebuild") != nullptr);
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    cli::ParsedArgs parsed = parser->parse({"prebuild", "--worker-only", "--image", "x:1", "--emsdk-image", "e:2"});
    CHECK_EQ(cli::command_path(parsed), std::string("prebuild"));
    CHECK(parsed.flag("worker_only"));
    CHECK_EQ(parsed.text_or("image", ""), std::string("x:1"));
    CHECK_EQ(parsed.text_or("emsdk_image", ""), std::string("e:2"));
    cli::ParsedArgs defaults = parser->parse({"prebuild"});
    CHECK(!defaults.flag("worker_only"));
    CHECK(!defaults.has("image"));
    CHECK(!defaults.has("emsdk_image"));  // not given: a missing Emscripten image is not an error
    // The options of the in-container executor and the tool mode are hidden and parse; --tool-fragment repeats.
    CHECK_EQ(parser->parse({"prebuild", "--image-id", "sha256:1", "--build-user", "1000:1000"}).text_or("image_id", ""), std::string("sha256:1"));
    CHECK_EQ(parser->parse({"prebuild", "--build-user", "501:20"}).text_or("build_user", ""), std::string("501:20"));
    CHECK_EQ(parser->parse({"prebuild", "--tool-fragment", "a.json", "--tool-fragment", "b.json", "--check"}).text_or("tool_fragment", ""),
             std::string("a.json\nb.json"));
    bool removed = false;  // the option of the retired arm64 carry is gone
    try {
        parser->parse({"prebuild", std::string("--drop-stale-") + "arm64"});
    } catch (const cli::ParseExit& exit) {
        removed = exit.code == 2;
    }
    CHECK(removed);
    CHECK(!defaults.flag("wasm_worker_only"));
    CHECK(!defaults.flag("check"));
    cli::ParsedArgs wasm = parser->parse({"prebuild", "--wasm-worker-only", "--check", "--emsdk-image", "e:2"});
    CHECK(wasm.flag("wasm_worker_only"));
    CHECK(wasm.flag("check"));
    CHECK_EQ(wasm.text_or("emsdk_image", ""), std::string("e:2"));
    CHECK(parser->parse({"prebuild", "--wasm-worker-only"}).flag("wasm_worker_only"));
    CHECK(!parser->parse({"prebuild", "--wasm-worker-only"}).flag("check"));
}

TEST(prebuild_flags_are_the_recorded_ones) {
    auto dump = [](const std::vector<std::string>& flags) { return json_dumps(Json(flags), DumpOptions::compact()); };
    CHECK_EQ(dump(windows_worker_flags()), std::string("[\"-std=c++17\",\"-O2\",\"-static\",\"-s\",\"-lshell32\"]"));
    CHECK_EQ(dump(linux_worker_flags()),
             std::string("[\"-std=c++17\",\"-O2\",\"-static-libstdc++\",\"-static-libgcc\",\"-s\",\"-ldl\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("windows-gcc", "nd2")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-DPLAYER_ND2\",\"-static\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("windows-clang", "normal_dist")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-static\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("linux-clang", "nd2")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-DPLAYER_ND2\","
                         "\"-static-libstdc++\",\"-static-libgcc\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("wasm", "nd2")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-fexceptions\","
                         "\"-sNODERAWFS=1\",\"-sALLOW_MEMORY_GROWTH=1\",\"-sENVIRONMENT=node\",\"-DPLAYER_ND2\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("windows-msvc", "normal_dist")),
             std::string("[\"/nologo\",\"/std:c++17\",\"/EHsc\",\"/O2\",\"/DNDEBUG\",\"/wd4996\",\"/utf-8\"]"));
    CHECK(!error_of([] { prebuilt_player_flags("macos-gcc", "nd2"); }).empty());
    // linux-arm64: the nofma ids add -ffp-contract=off, the worker too; the plain ids are the amd64 ones.
    CHECK_EQ(dump(prebuilt_player_flags("linux-gcc-nofma", "normal_dist")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-static-libstdc++\","
                         "\"-static-libgcc\",\"-ffp-contract=off\"]"));
    CHECK_EQ(dump(prebuilt_player_flags("linux-clang-nofma", "nd2")),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-DPLAYER_ND2\","
                         "\"-static-libstdc++\",\"-static-libgcc\",\"-ffp-contract=off\"]"));
    CHECK_EQ(dump(linux_arm64_worker_flags()),
             std::string("[\"-std=c++17\",\"-O2\",\"-ffp-contract=off\",\"-static-libstdc++\",\"-static-libgcc\",\"-s\",\"-ldl\"]"));
}

TEST(container_scripts_have_lf_endings_and_the_recorded_commands) {
    for (const std::string& script :
         {linux_worker_script(), linux_players_script(), wasm_players_script(), linux_loader_script(), linux_worker_script(true),
          linux_players_script(true), linux_loader_script(true)}) {
        CHECK(script.find('\r') == std::string::npos);
        CHECK(script.compare(0, 20, "set -euo pipefail\n") == 0 || script.compare(0, 17, "set -euo pipefail") == 0);
        CHECK(!script.empty() && script.back() == '\n');
    }
    // A script that uses a fixed work directory outside its mounts starts by removing it (both executors).
    CHECK(linux_loader_script().find("b=/tmp/loader-$COMP\nrm -rf \"$b\"\n") != std::string::npos);
    CHECK(linux_loader_script(true).find("b=/tmp/loader-$COMP\nrm -rf \"$b\"\n") != std::string::npos);
    CHECK(linux_worker_script().find("set -euo pipefail\nrm -rf /tmp/dc_worker\n") == 0);
    CHECK(linux_worker_script(true).find("set -euo pipefail\nrm -rf /tmp/dc_worker\n") == 0);
    // The amd64 and arm64 variants.
    CHECK(linux_worker_script().find("-ffp-contract=off") == std::string::npos);
    CHECK(linux_worker_script(true).find("-std=c++17 -O2 -ffp-contract=off -static-libstdc++ -static-libgcc -s") != std::string::npos);
    CHECK(linux_loader_script().find("objdump") == std::string::npos);
    CHECK(linux_loader_script(true).find("command -v objdump") != std::string::npos);
    CHECK(linux_loader_script(true).find("> \"$out/fma.txt\"") != std::string::npos);
    CHECK(linux_loader_script(true).find("-DCMAKE_C_FLAGS=$extra") != std::string::npos);
    CHECK(linux_players_script().find("linux-amd64-$comp") != std::string::npos &&
          linux_players_script().find("gcc-nofma") == std::string::npos);
    CHECK(linux_players_script(true).find("for comp in gcc clang gcc-nofma clang-nofma; do") != std::string::npos);
    CHECK(linux_players_script(true).find("-static-libstdc++ -static-libgcc $extra -o \"$dir/worker\"") != std::string::npos);
    CHECK(linux_players_script(true).find("linux-amd64") == std::string::npos);
    CHECK(linux_worker_script().find("-std=c++17 -O2 -static-libstdc++ -static-libgcc -s") != std::string::npos);
    CHECK(linux_worker_script().find("\"-DDC_WORKER_SOURCE_SHA256=\\\"$SOURCE_SHA\\\"\"") != std::string::npos);
    CHECK(linux_players_script().find("-static-libstdc++ -static-libgcc -o \"$dir/worker\"") != std::string::npos);
    CHECK(linux_players_script().find("-I /deps/json/include -I /deps/fpm/include") != std::string::npos);
    CHECK(wasm_players_script().find("-fexceptions -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node") != std::string::npos);
    CHECK(wasm_players_script().find("worker.js\" /out/smoke/wasm-container-$player.bin 640") != std::string::npos);
}

TEST(source_identity_comes_from_the_vendored_trees) {
    SourceIdentity identity = source_identity(repo());
    CHECK_EQ(identity.worker_sha256, sha256_lf_file(repo() / "src" / "worker" / "worker.cpp"));
    CHECK_EQ(identity.player_worker_sha256, sha256_lf_file(repo() / "src" / "worker" / "player.cpp"));
    Json revisions = read_json(repo() / "src" / "vendor" / "manifest.json");
    CHECK_EQ(identity.player_library["tree_sha256_lf"], revisions["digitalcurling"]["tree_sha256_lf"]);
    CHECK_EQ(identity.player_library["revision"], revisions["digitalcurling"]["revision"]);
    CHECK_EQ(identity.dependencies.size(), static_cast<std::size_t>(2));
    CHECK_EQ(identity.dependencies.begin().key(), std::string("fpm"));
    Json deps = read_json(repo() / "src" / "vendor" / "manifest.json");
    CHECK_EQ(identity.dependencies["fpm"]["commit"], deps["dependencies"]["fpm"]["pin"]);
    CHECK_EQ(identity.dependencies["nlohmann_json"]["git_tree"], deps["dependencies"]["nlohmann-json"]["git_tree"]);
    CHECK_EQ(identity.dependencies["fpm"]["include_tree_sha256_lf"].get<std::string>().size(), static_cast<std::size_t>(64));
    Json source = players_source_json(identity);
    CHECK_EQ(source.begin().key(), std::string("worker"));
    CHECK_EQ(source["worker"]["path"], Json("src/worker/player.cpp"));
    // The recorded manifest has the same identity for every source member that exists in the repository.
    Json manifest = read_json(repo() / "prebuilt" / "manifest.json");
    CHECK_EQ(manifest["players"]["source"]["player_library"], identity.player_library);
    CHECK_EQ(manifest["players"]["source"]["dependencies"], identity.dependencies);
}

TEST(identity_mismatch_names_the_first_difference) {
    SyntheticRoot synthetic;
    Json manifest = synthetic.manifest();
    SourceIdentity identity = synthetic.identity;
    CHECK(!identity_mismatch(manifest, identity, true));
    CHECK(!identity_mismatch(manifest, identity, false));

    SourceIdentity worker = identity;
    worker.worker_sha256 = std::string(64, 'a');
    std::optional<std::string> mismatch = identity_mismatch(manifest, worker, false);
    CHECK(mismatch && mismatch->find("worker.cpp changed (sha256 " + std::string(64, 'a') + ", manifest " + identity.worker_sha256 + ")") == 0);

    SourceIdentity player_worker = identity;
    player_worker.player_worker_sha256 = std::string(64, 'b');
    mismatch = identity_mismatch(manifest, player_worker, true);
    CHECK(mismatch && mismatch->find("src/worker/player.cpp changed") == 0);
    CHECK(!identity_mismatch(manifest, player_worker, false));  // the players are not rebuilt: not compared

    SourceIdentity tree = identity;
    tree.player_library["tree_sha256_lf"] = std::string(64, 'c');
    mismatch = identity_mismatch(manifest, tree, true);
    CHECK(mismatch && mismatch->find("vendored player library tree_sha256_lf") == 0);

    SourceIdentity pin = identity;
    pin.dependencies["nlohmann_json"]["commit"] = std::string(40, 'd');
    mismatch = identity_mismatch(manifest, pin, true);
    CHECK(mismatch && mismatch->find("nlohmann_json HEAD differs from the pin") == 0);

    SourceIdentity git_tree = identity;
    git_tree.dependencies["fpm"]["git_tree"] = std::string(40, 'e');
    mismatch = identity_mismatch(manifest, git_tree, true);
    CHECK(mismatch && mismatch->find("fpm Git tree id differs from the manifest") == 0);

    SourceIdentity include_tree = identity;
    include_tree.dependencies["fpm"]["include_tree_sha256_lf"] = std::string(64, 'f');
    mismatch = identity_mismatch(manifest, include_tree, true);
    CHECK(mismatch && mismatch->find("fpm include tree " + std::string(64, 'f') + " differs from the manifest") == 0);

    Json without_players = manifest;
    without_players.erase("players");
    mismatch = identity_mismatch(without_players, identity, true);
    CHECK(mismatch && mismatch->find("no players section") != std::string::npos);
}

TEST(manifest_merges_keep_the_other_members_and_order_the_entries_by_platform) {
    Json existing = Json::parse(R"({
        "schema": "dc-determinism-prebuilt-v2",
        "source": {"path": "src/worker/worker.cpp", "sha256": "old"},
        "binaries": {"windows-amd64": {"sha256": "w"}, "linux-amd64": {"sha256": "l"}},
        "players": {"source": {"x": 1}, "binaries": {"a/b": {"sha256": "1"}, "linux-amd64-gcc/nd2": {"sha256": "2"}, "wasm/nd2": {"sha256": "3"}}},
        "toolchain": {"windows-amd64": {"msvc": {"v": 1}, "gcc": {"v": 1}}, "linux-amd64": {"image": "old", "image_id": "sha256:old"}},
        "dc_determinism": {"binaries": {"linux-amd64": {"sha256": "keep"}}},
        "extra": {"carried": true}
    })");
    auto keys = [](const Json& object) {
        std::vector<std::string> names;
        for (auto it = object.begin(); it != object.end(); ++it) names.push_back(it.key());
        return names;
    };

    Json source = worker_source_json("new");
    Json binaries = Json::object();
    binaries["windows-amd64"] = worker_binary_json("p/w.exe", "nw", "g++", {"-O2"});
    binaries["linux-amd64"] = worker_binary_json("p/l", "nl", "g++", {"-O2"}, "img", "sha256:1");
    CHECK_EQ(json_dumps(binaries["linux-amd64"], DumpOptions::compact()),
             std::string("{\"path\":\"p/l\",\"sha256\":\"nl\",\"compiler\":\"g++\",\"flags\":[\"-O2\"],\"image\":\"img\",\"image_id\":\"sha256:1\"}"));
    CHECK(!binaries["windows-amd64"].contains("image"));

    // A worker-only update: source, binaries and the compiler it probed; players and the unknown members stay.
    ManifestUpdate worker_only;
    worker_only.source = source;
    worker_only.binaries = binaries;
    worker_only.toolchain = Json::parse(R"({"windows-amd64": {"gcc": {"v": 2}}, "linux-amd64": {"image": "img", "image_id": "sha256:1"}})");
    Json merged = merge_manifest(existing, worker_only);
    CHECK(keys(merged) == keys(existing));
    CHECK_EQ(merged["source"]["sha256"], Json("new"));
    CHECK_EQ(merged["binaries"], binaries);
    CHECK_EQ(merged["players"], existing["players"]);
    CHECK_EQ(merged["dc_determinism"], existing["dc_determinism"]);
    CHECK_EQ(merged["extra"], existing["extra"]);  // an unknown member is carried
    CHECK_EQ(merged["toolchain"]["windows-amd64"]["msvc"], Json::parse(R"({"v": 1})"));  // compiler by compiler
    CHECK_EQ(merged["toolchain"]["windows-amd64"]["gcc"], Json::parse(R"({"v": 2})"));
    CHECK_EQ(merged["toolchain"]["linux-amd64"]["image_id"], Json("sha256:1"));

    // A full Windows update: the entries of the prefixes are replaced, others kept, the order is by platform.
    Json players = Json::parse(R"({"source": {"x": 2}, "binaries": {"windows-amd64-msvc/nd2": {"sha256": "1"}, "linux-amd64-gcc/nd2": {"sha256": "2"}}})");
    ManifestUpdate full;
    full.source = source;
    full.binaries = binaries;
    full.players_source = players["source"];
    full.replaced_player_prefixes = {"windows-amd64-", "linux-amd64-"};
    full.player_entries = players["binaries"];
    Json windows_merged = merge_manifest(existing, full);
    CHECK(keys(windows_merged) == keys(existing));
    CHECK_EQ(windows_merged["players"]["source"], players["source"]);
    CHECK((keys(windows_merged["players"]["binaries"]) == std::vector<std::string>{"windows-amd64-msvc/nd2", "linux-amd64-gcc/nd2", "wasm/nd2", "a/b"}));
    CHECK_EQ(windows_merged["players"]["binaries"]["wasm/nd2"], existing["players"]["binaries"]["wasm/nd2"]);  // carried
    CHECK_EQ(windows_merged["dc_determinism"], existing["dc_determinism"]);
    // Without a previous manifest the members are written in the order of the schema.
    Json fresh = merge_manifest(Json::object(), full);
    CHECK((keys(fresh) == std::vector<std::string>{"schema", "source", "binaries", "players"}));
    CHECK_EQ(fresh["schema"], Json("dc-determinism-prebuilt-v2"));

    // A Linux host: its entries are replaced in place, the entries of the other platforms stay where they are, new ones
    // come after their platform.
    Json entries = Json::parse(R"({"linux-amd64-gcc/nd2": {"sha256": "new2"}, "linux-amd64-clang/nd2": {"sha256": "new3"}, "linux-amd64-clang/normal_dist": {"sha256": "new4"}})");
    ManifestUpdate linux_update;
    linux_update.binaries = Json::object();
    linux_update.binaries["linux-amd64"] = worker_binary_json("p/l", "nl", "g++", {}, "img", "id");
    linux_update.replaced_player_prefixes = {"linux-amd64-"};
    linux_update.player_entries = entries;
    linux_update.toolchain = Json::parse(R"({"linux-amd64": {"image": "img", "image_id": "id"}})");
    Json linux_merged = merge_manifest(existing, linux_update);
    CHECK_EQ(linux_merged["binaries"]["windows-amd64"], existing["binaries"]["windows-amd64"]);
    CHECK_EQ(linux_merged["binaries"]["linux-amd64"]["sha256"], Json("nl"));
    CHECK_EQ(linux_merged["source"], existing["source"]);
    CHECK((keys(linux_merged["players"]["binaries"]) ==
           std::vector<std::string>{"linux-amd64-gcc/nd2", "linux-amd64-clang/nd2", "linux-amd64-clang/normal_dist", "wasm/nd2", "a/b"}));
    CHECK_EQ(linux_merged["players"]["source"], existing["players"]["source"]);
    CHECK_EQ(linux_merged["toolchain"]["windows-amd64"], existing["toolchain"]["windows-amd64"]);
    CHECK_EQ(linux_merged["toolchain"]["linux-amd64"]["image_id"], Json("id"));

    // An arm64 host adds its platform after linux-amd64 and before wasm; toolchain.linux-arm64 appears.
    ManifestUpdate arm64;
    arm64.binaries = Json::object();
    arm64.binaries["linux-arm64"] = worker_binary_json("p/a", "na", "g++", {}, "arm", "sha256:arm");
    arm64.replaced_player_prefixes = {"linux-arm64-"};
    arm64.player_entries = Json::parse(R"({"linux-arm64-gcc/nd2": {"sha256": "a1"}, "linux-arm64-gcc-nofma/nd2": {"sha256": "a2"}})");
    arm64.toolchain = Json::parse(R"({"linux-arm64": {"image": "arm", "image_id": "sha256:arm"}})");
    Json arm64_merged = merge_manifest(existing, arm64);
    CHECK((keys(arm64_merged["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "linux-arm64"}));
    CHECK((keys(arm64_merged["players"]["binaries"]) ==
           std::vector<std::string>{"linux-amd64-gcc/nd2", "linux-arm64-gcc/nd2", "linux-arm64-gcc-nofma/nd2", "wasm/nd2", "a/b"}));
    CHECK((keys(arm64_merged["toolchain"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "linux-arm64"}));
    // ... and a Windows host that finds it stale removes exactly that: entries, players of the prefix and the toolchain member.
    ManifestUpdate removal;
    removal.removed_binaries = {"linux-arm64"};
    removal.replaced_player_prefixes = {"linux-arm64-"};
    removal.removed_toolchain = {"linux-arm64"};
    Json removed = merge_manifest(arm64_merged, removal);
    CHECK(removed["binaries"] == existing["binaries"] && removed["toolchain"] == existing["toolchain"]);
    CHECK(!removed["binaries"].contains("linux-arm64") && !removed["toolchain"].contains("linux-arm64"));
    CHECK((keys(removed["players"]["binaries"]) == std::vector<std::string>{"linux-amd64-gcc/nd2", "wasm/nd2", "a/b"}));
}

TEST(manifest_text_is_python_style_json_with_a_final_newline) {
    Json manifest = Json::parse(R"({"schema": "s", "list": ["a", "b"], "empty": [], "nested": {"k": "v<>"}})");
    std::string text = manifest_text(manifest);
    CHECK_EQ(text,
             std::string("{\n  \"schema\": \"s\",\n  \"list\": [\n    \"a\",\n    \"b\"\n  ],\n  \"empty\": [],\n"
                         "  \"nested\": {\n    \"k\": \"v<>\"\n  }\n}\n"));
    CHECK_EQ(parse_json_text(text, "x"), manifest);
    // The recorded manifest is written back byte for byte.
    std::string recorded = read_file(repo() / "prebuilt" / "manifest.json");
    CHECK_EQ(manifest_text(parse_json_text(recorded, "manifest")), recorded);
}

TEST(smoke_outputs_are_decoded_and_compared) {
    testing::TempDir dir;
    write_bytes(dir / "a.bin", pdet(1));
    write_bytes(dir / "b.bin", pdet(1));
    write_bytes(dir / "c.bin", pdet(1, 640, true));
    write_bytes(dir / "bad.bin", pdet(1, 640, false, false));
    write_bytes(dir / "junk.bin", "not a player output");

    SmokeReport good = check_smoke({{"normal_dist", {{"first", dir / "a.bin"}, {"second", dir / "b.bin"}}}});
    CHECK(!good.failed);
    CHECK_EQ(good.lines.size(), static_cast<std::size_t>(3));
    CHECK(good.lines[0].find("decoded normal_dist first: samples=163840 sha256=") == 0);
    CHECK_EQ(good.lines[2], std::string("compare normal_dist first vs second: different_samples=0 byte_identical=true"));

    SmokeReport different = check_smoke({{"normal_dist", {{"first", dir / "a.bin"}, {"other", dir / "c.bin"}}}});
    CHECK(!different.failed);  // a difference between platforms is reported, not an error
    CHECK_EQ(different.lines.back(), std::string("compare normal_dist first vs other: different_samples=1 byte_identical=false"));

    SmokeReport restore = check_smoke({{"nd2", {{"first", dir / "bad.bin"}}}});
    CHECK(restore.failed);
    CHECK(restore.lines[0].find("FAIL nd2 first: restore_failures=") == 0);

    SmokeReport junk = check_smoke({{"nd2", {{"first", dir / "a.bin"}, {"junk", dir / "junk.bin"}}}});
    CHECK(junk.failed);
    CHECK(junk.lines.back().find("FAIL nd2 junk: invalid player output ") == 0);

    CHECK(!error_of([&] { check_smoke({{"nd2", {{"first", dir / "missing.bin"}}}}); }).empty());
}

// --- the Windows host -----------------------------------------------------------------------------------------------

namespace {

// The flags a Windows build records: the recipe's flags plus the reproducibility ones (the fixed build path as <repo>).
std::vector<std::string> with_repro(std::vector<std::string> flags, const std::string& compiler) {
    if (!is_windows()) return flags;
    if (compiler == "msvc") {
        for (const char* flag : {"/Brepro", "/link", "/Brepro"}) flags.push_back(flag);
    } else {
        flags.push_back("-ffile-prefix-map=<repo>=.");
        flags.push_back("-Wl,--no-insert-timestamp");
    }
    return flags;
}

}  // namespace

TEST(windows_full_prebuild_rebuilds_every_entry_and_keeps_other_sections) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    write_bytes(synthetic.root / "prebuilt" / "players" / "stale" / "left-over.txt", "old player");
    Json before = synthetic.read();

    PrebuildOptions options;
    options.root = synthetic.root;
    run_windows_prebuild(options);

    Json manifest = synthetic.read();
    std::vector<std::string> top;
    for (auto it = manifest.begin(); it != manifest.end(); ++it) top.push_back(it.key());
    CHECK((top == std::vector<std::string>{"schema", "source", "binaries", "players", "toolchain", "dc_determinism"}));
    CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
    CHECK_EQ(manifest["source"], worker_source_json(synthetic.identity.worker_sha256));
    CHECK_EQ(manifest["players"]["source"], players_source_json(synthetic.identity));

    // dc_worker: g++ as the manifest has always recorded it (lower-case name), Docker image and id.
    Json windows = manifest["binaries"]["windows-amd64"];
    CHECK_EQ(windows["path"], Json("prebuilt/worker/windows-amd64/dc_worker.exe"));
    CHECK_EQ(windows["compiler"], Json("g++.exe (fake MSYS2) 16.2.0"));
    CHECK_EQ(windows["flags"], Json(with_repro(windows_worker_flags(), "gcc")));
    CHECK_EQ(windows["sha256"].get<std::string>(), sha256_file(synthetic.root / "prebuilt" / "worker" / "windows-amd64" / "dc_worker.exe"));
    Json linux_worker = manifest["binaries"]["linux-amd64"];
    CHECK_EQ(linux_worker["image"], Json("dc_determinism-cpu:local"));
    CHECK_EQ(linux_worker["image_id"], Json("sha256:id-of-dc_determinism-cpu:local"));
    CHECK_EQ(linux_worker["compiler"], Json("g++ (fake Ubuntu) 13.3.0"));
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "worker" / "linux-amd64" / "dc_worker"),
             "linux dc_worker " + synthetic.identity.worker_sha256);

    // players: 7 builds per player pair in the recorded order; the directory was replaced.
    std::vector<std::string> keys;
    for (auto it = manifest["players"]["binaries"].begin(); it != manifest["players"]["binaries"].end(); ++it) keys.push_back(it.key());
    CHECK((keys == std::vector<std::string>{
                       "windows-amd64-msvc/normal_dist", "windows-amd64-msvc/nd2", "windows-amd64-gcc/normal_dist",
                       "windows-amd64-gcc/nd2", "windows-amd64-clang/normal_dist", "windows-amd64-clang/nd2",
                       "linux-amd64-gcc/normal_dist", "linux-amd64-gcc/nd2", "linux-amd64-clang/normal_dist",
                       "linux-amd64-clang/nd2", "wasm/normal_dist", "wasm/nd2"}));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "players" / "stale"));
    for (auto it = manifest["players"]["binaries"].begin(); it != manifest["players"]["binaries"].end(); ++it) {
        const Json& entry = it.value();
        fs::path installed = synthetic.root / path_from_utf8(entry["path"].get<std::string>());
        CHECK_EQ(sha256_file(installed), entry["sha256"].get<std::string>());
        CHECK(entry["flags"].is_array() && !entry["flags"].empty());
    }
    CHECK_EQ(manifest["players"]["binaries"]["windows-amd64-gcc/nd2"]["flags"],
             Json(with_repro(prebuilt_player_flags("windows-gcc", "nd2"), "gcc")));
    CHECK_EQ(manifest["players"]["binaries"]["windows-amd64-msvc/nd2"]["flags"],
             Json(with_repro(prebuilt_player_flags("windows-msvc", "nd2"), "msvc")));
    CHECK_EQ(manifest["players"]["binaries"]["linux-amd64-clang/normal_dist"]["flags"],
             Json(prebuilt_player_flags("linux-clang", "normal_dist")));
    CHECK_EQ(manifest["players"]["binaries"]["wasm/normal_dist"]["flags"], Json(prebuilt_player_flags("wasm", "normal_dist")));
    Json msvc = manifest["players"]["binaries"]["windows-amd64-msvc/nd2"];
    CHECK_EQ(msvc["compiler"], Json("msvc fake 1.0"));
    CHECK_EQ(msvc["platform"], Json("windows-amd64"));
    CHECK_EQ(msvc["compiler_id"], Json("msvc"));
    CHECK_EQ(msvc["player"], Json("nd2"));
    CHECK_EQ(manifest["players"]["binaries"]["windows-amd64-gcc/nd2"]["compiler"], Json("g++.exe (fake MSYS2) 16.2.0"));
    Json linux_player = manifest["players"]["binaries"]["linux-amd64-clang/nd2"];
    CHECK_EQ(linux_player["compiler"], Json("Ubuntu clang version fake"));
    std::vector<std::string> linux_keys;
    for (auto it = linux_player.begin(); it != linux_player.end(); ++it) linux_keys.push_back(it.key());
    CHECK((linux_keys == std::vector<std::string>{"path", "sha256", "platform", "compiler_id", "compiler", "player", "flags",
                                                  "image_id", "image"}));
    Json wasm = manifest["players"]["binaries"]["wasm/nd2"];
    CHECK_EQ(wasm["compiler"], Json("emcc (fake) 6.0.9"));
    CHECK_EQ(wasm["image"], Json("emscripten/emsdk:6.0.9"));
    CHECK_EQ(wasm["sidecars"]["worker.wasm"]["path"], Json("prebuilt/players/wasm/nd2/worker.wasm"));
    CHECK_EQ(wasm["sidecars"]["worker.wasm"]["sha256"].get<std::string>(),
             sha256_file(synthetic.root / "prebuilt" / "players" / "wasm" / "nd2" / "worker.wasm"));
    std::vector<std::string> wasm_keys;
    for (auto it = wasm.begin(); it != wasm.end(); ++it) wasm_keys.push_back(it.key());
    CHECK((wasm_keys == std::vector<std::string>{"path", "sha256", "platform", "compiler_id", "compiler", "player", "flags",
                                                 "image", "image_id", "sidecars"}));

    // The toolchains, the commands and the Docker calls.
    CHECK(contains(world.probed, "player/gcc") && contains(world.probed, "player/msvc") && contains(world.probed, "player/clang"));
    CHECK_EQ(world.builds.size(), static_cast<std::size_t>(1 + 6));  // dc_worker + three compilers x two players
    CHECK_EQ(world.cmake_calls.size(), static_cast<std::size_t>(3 * 2));  // three windows loaders: configure and build
    const std::vector<std::string>& worker_command = world.builds[0];
    CHECK_EQ(worker_command[0], std::string("/fake/gcc/cxx.EXE"));
    CHECK(contains(worker_command, "-lshell32") && contains(worker_command, "-static"));
    auto response = std::find_if(worker_command.begin(), worker_command.end(),
                                 [](const std::string& item) { return !item.empty() && item[0] == '@'; });
    CHECK(response != worker_command.end() && ends_with(*response, "defs.rsp"));
    if (is_windows()) {  // the reproducibility flags of the Windows builds
        CHECK(contains(worker_command, "-Wl,--no-insert-timestamp"));
        CHECK(std::any_of(worker_command.begin(), worker_command.end(),
                          [](const std::string& item) { return item.compare(0, 18, "-ffile-prefix-map=") == 0; }));
        CHECK(contains(world.builds[1], "/Brepro") && contains(world.builds[1], "/link"));
    }
    const std::vector<std::string>& msvc_command = world.builds[1];
    CHECK(contains(msvc_command, "/nologo"));
    CHECK(std::any_of(msvc_command.begin(), msvc_command.end(), [](const std::string& item) { return item.compare(0, 4, "/Fe:") == 0; }));
    CHECK(!world.build_cwds[1].empty());   // msvc runs in the output directory (object files)
    CHECK(world.build_cwds[3].empty());    // gcc does not
    // MSYS2 is removed from PATH for the static dc_worker and the gcc/clang smoke runs, not for msvc and wasm.
    for (std::size_t i = 0; i < world.programs.size(); ++i) {
        const std::vector<std::string>& argv = world.programs[i];
        if (argv.size() == 3 && argv[0].find("windows-amd64-msvc") != std::string::npos) CHECK(!world.without_msys_flags[i]);
        if (argv.size() == 3 && argv[0].find("windows-amd64-gcc") != std::string::npos) CHECK(world.without_msys_flags[i]);
        if (argv.size() == 4 && argv[0] == "node") CHECK(!world.without_msys_flags[i]);
        if (argv.size() == 2 && argv[1] == "--self-test") CHECK(world.without_msys_flags[i]);
    }
    std::size_t runs = 0;
    std::size_t wasm_worker_runs = 0;
    for (const auto& call : world.docker_calls) {
        if (call[0] != "run" || contains(call, "chown")) continue;
        if (call.back() == "/repo/src/worker/wasm/build-wasm-worker.sh") {  // the wasm dc_worker: the repository itself
            ++wasm_worker_runs;
            CHECK(std::any_of(call.begin(), call.end(), [](const std::string& item) { return ends_with(item, ":/repo:ro"); }));
            continue;
        }
        ++runs;
        CHECK_EQ(call.back(), std::string("/build.sh"));
        CHECK(call.size() > 5 && call[1] == "--rm" && call[2] == "--entrypoint" && call[3] == "bash");
        CHECK(!contains(call, "-u") || call[4] == "-u");
        CHECK(std::any_of(call.begin(), call.end(), [](const std::string& item) { return ends_with(item, ":/src:ro"); }));
    }
    CHECK_EQ(runs, static_cast<std::size_t>(5));  // linux dc_worker, two linux loaders, linux players, wasm players
    CHECK_EQ(wasm_worker_runs, static_cast<std::size_t>(1));
    // The WebAssembly dc_worker is installed with its own manifest, after being verified against the host node.
    fs::path bundle = synthetic.root / "prebuilt" / "worker" / "wasm";
    CHECK_EQ(read_file(bundle / "dc_worker.js"), std::string("wasm worker js v1"));
    CHECK_EQ(read_file(bundle / "dc_worker.wasm"), std::string("wasm worker module v1"));
    Json wasm_worker = read_json(bundle / "manifest.json");
    CHECK_EQ(wasm_worker["schema"], Json("dc-determinism-wasm-worker-v1"));
    CHECK_EQ(wasm_worker["worker"]["sha256"].get<std::string>(), sha256_file(bundle / "dc_worker.js"));
    CHECK_EQ(wasm_worker["source"]["sha256"].get<std::string>(), synthetic.identity.worker_sha256);
    // The shipped loaders: three Windows and two Linux libraries, manifests without the removed `observer` member.
    Json windows_loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    Json linux_loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "linux-amd64" / "manifest.json");
    CHECK(!windows_loaders.contains("observer") && !linux_loaders.contains("observer"));
    CHECK_EQ(windows_loaders["schema"], Json("dc-prebuilt-windows-amd64-v1"));
    CHECK_EQ(linux_loaders["schema"], Json("dc-prebuilt-linux-amd64-v1"));
    CHECK_EQ(linux_loaders["image_id"], Json("sha256:id-of-dc_determinism-cpu:local"));
    CHECK_EQ(windows_loaders["source"], linux_loaders["source"]);
    CHECK_EQ(windows_loaders["dependencies"], linux_loaders["dependencies"]);
    // The written shape: `source` is {revision, git_tree, tree_sha256_lf}, `dependencies` the archive hashes in the
    // order fpm, nlohmann-json, box2d, xxhash; the wasm manifest records the same identity.
    const std::vector<std::string> source_keys = {"revision", "git_tree", "tree_sha256_lf"};
    const std::vector<std::string> dependency_keys = {"fpm", "nlohmann-json", "box2d", "xxhash"};
    for (const Json* block : {&windows_loaders, &linux_loaders}) {
        CHECK(keys_of((*block)["source"]) == source_keys);
        CHECK(keys_of((*block)["dependencies"]) == dependency_keys);
    }
    CHECK(keys_of(wasm_worker["simulator"]["source"]) == source_keys);
    CHECK(keys_of(wasm_worker["simulator"]["dependencies"]) == dependency_keys);
    Json vendor_manifest = read_json(repo() / "src" / "vendor" / "manifest.json");
    CHECK_EQ(windows_loaders["dependencies"]["nlohmann-json"],
             vendor_manifest["dependencies"]["nlohmann-json"]["upstream_archive_sha256"]);
    // The loaders and the wasm worker are built from the same sources.
    auto loader_block = [](const Json& source, const Json& dependencies) {
        Json prebuilt = Json::object();
        prebuilt["source"] = source;
        prebuilt["dependencies"] = dependencies;
        Json info = Json::object();
        info["origin"] = "prebuilt";
        info["dc_prebuilt"] = prebuilt;
        return info;
    };
    Json windows_block = loader_block(windows_loaders["source"], windows_loaders["dependencies"]);
    CHECK(check_source_identity(windows_block, loader_block(linux_loaders["source"], linux_loaders["dependencies"]))
              ["verified"].get<bool>());
    CHECK(check_source_identity(windows_block, loader_block(wasm_worker["simulator"]["source"],
                                                             wasm_worker["simulator"]["dependencies"]))["verified"].get<bool>());
    CHECK_EQ(windows_loaders["libraries"]["msvc"]["filename"], Json("digitalcurling_plugin_loader.dll"));
    CHECK_EQ(windows_loaders["libraries"]["gcc"]["filename"],
             Json(is_windows() ? "libdigitalcurling_plugin_loader.dll" : "libdigitalcurling_plugin_loader.so"));
    CHECK_EQ(linux_loaders["libraries"]["clang"]["filename"], Json("libdigitalcurling_plugin_loader.so"));
    CHECK_EQ(linux_loaders["libraries"]["gcc"]["build"]["compiler_version"], Json("g++ (fake Ubuntu) 13.3.0"));
    // ldd's load addresses change from run to run and are not recorded (the check compares the manifest).
    CHECK_EQ(linux_loaders["libraries"]["gcc"]["build"]["runtime_dependencies"],
             Json("\tlinux-vdso.so.1\n\tlibc.so.6 => /lib/x86_64-linux-gnu/libc.so.6\n"));
    for (const char* compiler : {"msvc", "gcc", "clang"}) {
        const Json& library = windows_loaders["libraries"][compiler];
        CHECK_EQ(library["sha256"].get<std::string>(),
                 sha256_file(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / compiler /
                             path_from_utf8(library["filename"].get<std::string>())));
    }
    // The native dc_worker builds come first, then the wasm dc_worker, then the player builds.
    std::vector<std::string> order;
    for (const auto& call : world.docker_calls) {
        if (call[0] != "run" || contains(call, "chown")) continue;
        order.push_back(call.back() == "/repo/src/worker/wasm/build-wasm-worker.sh" ? "wasm-worker" : "script");
    }
    CHECK((order == std::vector<std::string>{"script", "script", "script", "wasm-worker", "script", "script"}));
    // The player containers get the vendored include trees and the vendored player library.
    for (const std::string& script : world.scripts) CHECK(script.find('\r') == std::string::npos);
    bool include_mounts = false;
    for (const auto& call : world.docker_calls) {
        include_mounts = include_mounts ||
                         std::any_of(call.begin(), call.end(), [](const std::string& item) {
                             return ends_with(item, "fpm" + std::string(1, fs::path::preferred_separator) + "include:/deps/fpm/include:ro");
                         });
    }
    CHECK(include_mounts);
}

TEST(windows_worker_only_changes_source_and_binaries) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    Json before = synthetic.read();
    write_bytes(synthetic.root / "prebuilt" / "players" / "keep.txt", "kept");

    PrebuildOptions options;
    options.root = synthetic.root;
    options.worker_only = true;
    options.image = "other-image:1";
    run_windows_prebuild(options);

    Json manifest = synthetic.read();
    CHECK_EQ(manifest["players"], before["players"]);
    CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
    CHECK_EQ(manifest["schema"], before["schema"]);
    CHECK_EQ(manifest["binaries"]["linux-amd64"]["image"], Json("other-image:1"));
    CHECK(manifest["binaries"]["windows-amd64"]["sha256"] != before["binaries"]["windows-amd64"]["sha256"]);
    CHECK(manifest["binaries"]["linux-amd64"]["sha256"] != before["binaries"]["linux-amd64"]["sha256"]);
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "players" / "keep.txt"), std::string("kept"));
    CHECK_EQ(world.builds.size(), static_cast<std::size_t>(1));
    CHECK(!contains(world.probed, "player/msvc"));
    std::size_t runs = 0;
    for (const auto& call : world.docker_calls) runs += call[0] == "run" ? 1 : 0;
    CHECK_EQ(runs, static_cast<std::size_t>(1));
    // The worker-only mode needs the player sources of neither the manifest nor the emsdk image.
    for (const auto& call : world.docker_calls) CHECK(!contains(call, "emscripten/emsdk:6.0.9"));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm"));
    CHECK(world.wasm_builds.empty());
}

TEST(windows_prebuild_stops_on_a_bad_player_output_and_leaves_the_install_alone) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.bad_restore = true;
    std::string before = synthetic.text();
    PrebuildOptions options;
    options.root = synthetic.root;
    std::string message = error_of([&] { run_windows_prebuild(options); });
    CHECK(message.find("smoke outputs failed") != std::string::npos);
    CHECK_EQ(synthetic.text(), before);
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "windows-amd64" / "dc_worker.exe"));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm"));
}

TEST(windows_prebuild_reports_a_failing_container_and_a_wrong_version) {
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.fail_run = true;
        PrebuildOptions options;
        options.root = synthetic.root;
        options.worker_only = true;
        std::string message = error_of([&] { run_windows_prebuild(options); });
        CHECK(message.find("docker build of the linux dc_worker failed (exit code 3):\nthe container failed") == 0);
    }
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.wrong_linux_version = true;
        PrebuildOptions options;
        options.root = synthetic.root;
        options.worker_only = true;
        CHECK_EQ(error_of([&] { run_windows_prebuild(options); }), std::string("linux --version mismatch"));
    }
    {
        SyntheticRoot synthetic;
        FakeWorld world("not-the-worker-sha");
        PrebuildOptions options;
        options.root = synthetic.root;
        options.worker_only = true;
        CHECK(error_of([&] { run_windows_prebuild(options); }).find("windows --version mismatch") == 0);
    }
}

// --- the WebAssembly dc_worker ---------------------------------------------------------------------------------------

namespace {

// The files below prebuilt/ that differ between two snapshots (added or changed).
std::vector<std::string> changed_files(const std::map<std::string, std::string>& before,
                                       const std::map<std::string, std::string>& after) {
    std::vector<std::string> changed;
    for (const auto& item : after) {
        auto old = before.find(item.first);
        if (old == before.end() || old->second != item.second) changed.push_back(item.first);
    }
    for (const auto& item : before) {
        if (!after.count(item.first)) changed.push_back(item.first);
    }
    return changed;
}

PrebuildOptions wasm_options(const SyntheticRoot& synthetic) {
    PrebuildOptions options;
    options.root = synthetic.root;
    options.wasm_worker_only = true;
    return options;
}

}  // namespace

TEST(wasm_worker_options_follow_the_rules) {
    PrebuildOptions options;
    CHECK(error_of([&] { check_prebuild_options(options); }).empty());
    options.check = true;
    CHECK(error_of([&] { check_prebuild_options(options); }).empty());  // every mode has a check
    options.wasm_worker_only = true;
    CHECK(error_of([&] { check_prebuild_options(options); }).empty());
    options.emsdk_image = "other/emsdk:1";
    CHECK(error_of([&] { check_prebuild_options(options); }).empty());
    options.worker_only = true;
    CHECK(error_of([&] { check_prebuild_options(options); }).find("--wasm-worker-only and --worker-only exclude each other") == 0);
    options.worker_only = false;
    options.check = false;
    options.image = "x:1";
    CHECK(error_of([&] { check_prebuild_options(options); }).find("--image does not apply to --wasm-worker-only") == 0);
    // The tool mode builds nothing: no other option goes with it but --check.
    PrebuildOptions tool;
    tool.tool_fragments = {"a.json"};
    CHECK(error_of([&] { check_prebuild_options(tool); }).empty());
    tool.check = true;
    CHECK(error_of([&] { check_prebuild_options(tool); }).empty());
    for (const std::function<void(PrebuildOptions&)>& change :
         std::vector<std::function<void(PrebuildOptions&)>>{[](PrebuildOptions& o) { o.worker_only = true; },
                                                              [](PrebuildOptions& o) { o.wasm_worker_only = true; },
                                                              [](PrebuildOptions& o) { o.image = "x:1"; },
                                                              [](PrebuildOptions& o) { o.emsdk_image_explicit = true; },
                                                              [](PrebuildOptions& o) { o.image_id = "sha256:1"; },
                                                              [](PrebuildOptions& o) { o.build_user = "1:1"; }}) {
        PrebuildOptions mixed = tool;
        change(mixed);
        CHECK(error_of([&] { check_prebuild_options(mixed); }).find("--tool-fragment builds nothing") == 0);
    }

    // run_prebuild applies the rules before it does anything.
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions bad;
    bad.root = synthetic.root;
    bad.wasm_worker_only = true;
    bad.worker_only = true;
    CHECK(error_of([&] { run_prebuild(bad); }).find("--wasm-worker-only and --worker-only exclude each other") == 0);
    CHECK(world.docker_calls.empty());
}

TEST(wasm_worker_only_installs_the_bundle_and_touches_nothing_else) {
    SyntheticRoot synthetic;
    synthetic.add_arm64();
    FakeWorld world(synthetic.identity.worker_sha256);
    std::map<std::string, std::string> before = synthetic.snapshot();
    std::string manifest_before = synthetic.text();

    CHECK_EQ(run_prebuild(wasm_options(synthetic)), 0);
    CHECK((changed_files(before, synthetic.snapshot()) ==
           std::vector<std::string>{"worker/wasm/dc_worker.js", "worker/wasm/dc_worker.wasm", "worker/wasm/manifest.json"}));
    CHECK_EQ(synthetic.text(), manifest_before);  // prebuilt/manifest.json is not touched

    fs::path bundle = synthetic.root / "prebuilt" / "worker" / "wasm";
    CHECK_EQ(read_file(bundle / "dc_worker.js"), std::string("wasm worker js v1"));
    Json manifest = read_json(bundle / "manifest.json");
    CHECK_EQ(manifest_text(manifest), read_file(bundle / "manifest.json"));  // Python-style JSON, final newline
    CHECK((keys_of(manifest) == std::vector<std::string>{"schema", "worker", "glue", "source", "simulator", "verified_trees",
                                                          "build", "toolchain", "flags", "smoke"}));
    CHECK_EQ(manifest["schema"], Json("dc-determinism-wasm-worker-v1"));
    std::string js_sha = sha256_file(bundle / "dc_worker.js");
    std::string wasm_sha = sha256_file(bundle / "dc_worker.wasm");
    CHECK((keys_of(manifest["worker"]) == std::vector<std::string>{"path", "sha256", "sidecars", "runtime", "node_min"}));
    CHECK_EQ(manifest["worker"]["path"], Json("prebuilt/worker/wasm/dc_worker.js"));
    CHECK_EQ(manifest["worker"]["sha256"], Json(js_sha));
    CHECK_EQ(manifest["worker"]["sidecars"]["dc_worker.wasm"], Json(wasm_sha));
    CHECK_EQ(manifest["worker"]["runtime"], Json("node"));
    CHECK_EQ(manifest["worker"]["node_min"], Json("v18"));
    CHECK((keys_of(manifest["glue"]) == std::vector<std::string>{"path", "sha256", "note"}));
    CHECK_EQ(manifest["glue"]["sha256"], Json(js_sha));
    CHECK_EQ(manifest["source"]["path"], Json("src/worker/worker.cpp"));
    CHECK_EQ(manifest["source"]["sha256"], Json(synthetic.identity.worker_sha256));
    Json loader = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    CHECK_EQ(manifest["simulator"]["source"], loader["source"]);
    CHECK_EQ(manifest["simulator"]["dependencies"], loader["dependencies"]);
    CHECK_EQ(manifest["verified_trees"]["simulator"]["path"], Json("src/vendor/digitalcurling"));
    CHECK_EQ(manifest["verified_trees"]["simulator"]["tree_sha256_lf"], loader["source"]["tree_sha256_lf"]);
    CHECK((keys_of(manifest["verified_trees"]["dependencies"]) ==
           std::vector<std::string>{"fpm", "nlohmann-json", "box2d", "xxhash"}));
    CHECK((keys_of(manifest["verified_trees"]["dependencies"]["box2d"]) ==
           std::vector<std::string>{"pin", "git_tree", "upstream_archive_sha256", "tree_sha256_lf"}));
    CHECK_EQ(manifest["verified_trees"]["dependencies"]["box2d"]["tree_sha256_lf"],
             Json(tree_sha256_lf(synthetic.root / "src" / "vendor" / "box2d")));
    CHECK((keys_of(manifest["build"]) == std::vector<std::string>{"override_sha256", "shim_sha256", "smoke_input", "script"}));
    CHECK_EQ(manifest["build"]["smoke_input"]["path"], Json("src/worker/wasm/smoke-input.bin"));
    CHECK_EQ(manifest["build"]["smoke_input"]["sha256"],
             Json(sha256_file(synthetic.root / "src" / "worker" / "wasm" / "smoke-input.bin")));
    CHECK_EQ(manifest["build"]["script"], Json("src/worker/wasm/build-wasm-worker.sh"));
    CHECK((keys_of(manifest["toolchain"]) ==
           std::vector<std::string>{"image", "image_id", "repo_digests", "emxx", "emxx_version_text", "cmake", "node"}));
    CHECK_EQ(manifest["toolchain"]["image"], Json("emscripten/emsdk:6.0.9"));
    CHECK_EQ(manifest["toolchain"]["image_id"], Json("sha256:id-of-emscripten/emsdk:6.0.9"));
    CHECK_EQ(manifest["toolchain"]["repo_digests"],
             Json::parse(R"(["emscripten/emsdk:6.0.9@sha256:digest-of-emscripten/emsdk:6.0.9"])"));
    CHECK_EQ(manifest["toolchain"]["emxx"], Json("emcc (fake) 6.0.9"));
    CHECK((keys_of(manifest["flags"]) == std::vector<std::string>{"configure", "link", "cmake_cache", "fp_contract"}));
    CHECK_EQ(manifest["flags"]["link"][2], Json("<repo>/src/worker/wasm/dcw_wasm_shim.h"));
    CHECK((keys_of(manifest["smoke"]) == std::vector<std::string>{"version", "self_test", "seconds_per_frame", "shots",
                                                                   "container_node", "host_node"}));
    CHECK_EQ(manifest["smoke"]["version"], Json("dc_worker " + synthetic.identity.worker_sha256));
    CHECK_EQ(manifest["smoke"]["seconds_per_frame"], Json::parse(R"({"fcv1": "0.001", "fcv1plus": "0.0005"})"));
    CHECK_EQ(manifest["smoke"]["shots"]["fcv1plus"], Json::parse(R"(["0 ok fcv1plus", "1 ok fcv1plus", "2 ok fcv1plus"])"));
    CHECK_EQ(manifest["smoke"]["container_node"], Json("v24.0.0"));
    CHECK_EQ(manifest["smoke"]["host_node"], Json("v26.0.0"));

    // One build container on the Emscripten image (the repository read-only, the script in it), then the hand-over of
    // the scratch directory; the host smoke runs the three shots through stdin; no other image is used.
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(1));
    const std::vector<std::string>& call = world.wasm_builds[0];
    CHECK((call[0] == "run" && call[1] == "--rm" && call[2] == "--entrypoint" && call[3] == "bash"));
    CHECK(std::any_of(call.begin(), call.end(), [](const std::string& item) { return ends_with(item, ":/repo:ro"); }));
    CHECK(std::any_of(call.begin(), call.end(), [](const std::string& item) { return ends_with(item, ":/out"); }));
    CHECK(contains(call, "emscripten/emsdk:6.0.9"));
    CHECK_EQ(call.back(), std::string("/repo/src/worker/wasm/build-wasm-worker.sh"));
    CHECK(world.builds.empty() && world.probed.empty());  // no native toolchain is involved
    std::size_t chowns = 0;
    for (const auto& docker_call : world.docker_calls) {
        if (contains(docker_call, "chown")) {
            ++chowns;
            CHECK(contains(docker_call, "1000:1000") && contains(docker_call, "-R"));
        } else {
            CHECK(contains(docker_call, "emscripten/emsdk:6.0.9"));
        }
    }
    CHECK_EQ(chowns, static_cast<std::size_t>(1));
    CHECK_EQ(world.program_inputs.size(), static_cast<std::size_t>(2));  // fcv1 and fcv1plus
    for (const std::string& input : world.smoke_inputs) CHECK_EQ(input, std::string("0\n1\n2\n"));
    CHECK(contains(world.program_inputs[0], "--max-steps") && contains(world.program_inputs[0], "60000"));
    CHECK_EQ(world.program_inputs[0][5], std::string("fcv1"));
    CHECK_EQ(world.program_inputs[1][5], std::string("fcv1plus"));
}

TEST(wasm_worker_check_compares_with_the_installed_bundle_and_installs_nothing) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions options = wasm_options(synthetic);
    CHECK_EQ(run_prebuild(options), 0);
    std::map<std::string, std::string> installed = synthetic.snapshot();

    options.check = true;
    CHECK_EQ(run_prebuild(options), 0);  // the same sources and image give the same bytes
    CHECK(synthetic.snapshot() == installed);

    world.wasm_content = "v2";  // another build: the bundles differ
    CHECK_EQ(run_prebuild(options), 1);
    CHECK(synthetic.snapshot() == installed);  // and nothing is installed
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "worker" / "wasm" / "dc_worker.js"), std::string("wasm worker js v1"));
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(3));

    // The comparison is with the installed files, not with what their manifest records.
    world.wasm_content = "v1";
    write_bytes(synthetic.root / "prebuilt" / "worker" / "wasm" / "dc_worker.wasm", "tampered");
    CHECK_EQ(run_prebuild(options), 1);

    // No installed bundle: the check reports that (the toolchain it records is missing too, which is looked at first).
    fs::remove_all(synthetic.root / "prebuilt" / "worker" / "wasm");
    CHECK_EQ(run_prebuild(options), 2);
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm"));
}

TEST(prebuild_refuses_other_schema_names) {
    {
        // build-info.json of the wasm container
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.wasm_info_schema = "dc-determinism-wasm-build-info-v2";
        CHECK(!error_of([&] { run_prebuild(wasm_options(synthetic)); }).empty());
    }
    {
        // prebuilt/manifest.json
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        Json manifest = synthetic.read();
        manifest["schema"] = "dc-determinism-prebuilt-v3";
        synthetic.write_manifest(manifest);
        PrebuildOptions options;
        options.root = synthetic.root;
        options.worker_only = true;
        options.image = "other-image:1";
        CHECK(!error_of([&] { run_windows_prebuild(options); }).empty());
    }
    {
        // a full Windows rebuild checks the recorded schema too, before anything is built
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        Json manifest = synthetic.read();
        manifest["schema"] = "dc-determinism-prebuilt-v3";
        synthetic.write_manifest(manifest);
        PrebuildOptions options;
        options.root = synthetic.root;
        options.image = "other-image:1";
        std::map<std::string, std::string> before = synthetic.snapshot();
        CHECK_EQ(error_of([&] { run_windows_prebuild(options); }), std::string("unexpected manifest schema: dc-determinism-prebuilt-v3"));
        CHECK(synthetic.snapshot() == before);
    }
}

TEST(wasm_worker_sources_are_verified_before_anything_is_built) {
    struct Case {
        const char* name;
        std::function<void(SyntheticRoot&)> sabotage;
        const char* fragment;
    };
    auto edit_json = [](SyntheticRoot& s, const std::string& relative, const std::function<void(Json&)>& change) {
        fs::path path = s.root / path_from_utf8(relative);
        Json value = read_json(path);
        change(value);
        write_bytes(path, manifest_text(value));
    };
    std::vector<Case> cases = {
        {"worker.cpp is newer than prebuilt/manifest.json",
         [](SyntheticRoot& s) { write_bytes(s.root / "src" / "worker" / "worker.cpp", "int newer;\n"); },
         "stale prebuilt/manifest.json: it records "},
        {"the simulator tree changed",
         [](SyntheticRoot& s) { write_bytes(s.root / "src" / "vendor" / "digitalcurling" / "stray.txt", "x"); },
         "src/vendor/digitalcurling does not match the loader manifests: tree sha256 is "},
        {"the loader manifests disagree about the dependencies",
         [&](SyntheticRoot& s) {
             edit_json(s, "prebuilt/loaders/linux-amd64/manifest.json", [](Json& d) { d["dependencies"]["box2d"] = "other"; });
         },
         "the windows-amd64 and linux-amd64 loader manifests disagree about 'dependencies'"},
        {"the loader manifests disagree about the source",
         [&](SyntheticRoot& s) {
             edit_json(s, "prebuilt/loaders/linux-amd64/manifest.json", [](Json& d) { d["source"]["revision"] = "other"; });
         },
         "the windows-amd64 and linux-amd64 loader manifests disagree about 'source'"},
        {"a loader dependency tree changed",
         [](SyntheticRoot& s) { write_bytes(s.root / "src" / "vendor" / "box2d" / "stray.txt", "x"); },
         "dependency box2d: src/vendor/box2d tree sha256 is "},
        {"a dependency archive differs from the loader manifests",
         [&](SyntheticRoot& s) {
             edit_json(s, "src/vendor/manifest.json",
                       [](Json& d) { d["dependencies"]["xxhash"]["upstream_archive_sha256"] = "other"; });
         },
         "dependency xxhash: src/vendor/manifest.json records upstream archive other, the loader manifests "},
        {"a dependency has no entry",
         [&](SyntheticRoot& s) {
             edit_json(s, "src/vendor/manifest.json", [](Json& d) { d["dependencies"].erase("xxhash"); });
         },
         "src/vendor/manifest.json has no entry for xxhash"},
        {"no loader manifest", [](SyntheticRoot& s) { fs::remove(s.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json"); },
         "cannot read "},
    };
    for (const Case& item : cases) {
        SyntheticRoot synthetic;
        item.sabotage(synthetic);
        FakeWorld world(synthetic.identity.worker_sha256);
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_prebuild(wasm_options(synthetic)); });
        CHECK(message.find(item.fragment) != std::string::npos);
        CHECK(synthetic.snapshot() == before);
        CHECK(world.docker_calls.empty());  // refused before the first Docker call
        CHECK(world.wasm_builds.empty());
    }

    // A loader manifest of linux-arm64 is optional, but when present it must agree with the others.
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    fs::create_directories(synthetic.root / "prebuilt" / "loaders" / "linux-arm64");
    fs::copy_file(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json",
                  synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json");
    CHECK_EQ(run_prebuild(wasm_options(synthetic)), 0);
    edit_json(synthetic, "prebuilt/loaders/linux-arm64/manifest.json", [](Json& d) { d["dependencies"]["xxhash"] = "other"; });
    CHECK(error_of([&] { run_prebuild(wasm_options(synthetic)); })
              .find("the windows-amd64 and linux-arm64 loader manifests disagree about 'dependencies'") == 0);
}

TEST(wasm_worker_build_stops_when_the_container_or_the_host_smoke_tests_disagree) {
    struct Case {
        const char* name;
        std::function<void(FakeWorld&)> setup;
        const char* fragment;
    };
    std::vector<Case> cases = {
        {"the container fails", [](FakeWorld& w) { w.fail_run = true; }, "docker wasm worker build failed (exit code 3):\nthe container failed"},
        {"the container built another worker.cpp", [](FakeWorld& w) { w.wasm_other_source = true; },
         "the container built another worker.cpp than the host verified"},
        {"the container smoke test reports another version", [](FakeWorld& w) { w.wasm_wrong_container_version = true; },
         "container smoke test: version 'dc_worker 0', self-test 'OK'"},
        {"the smoke input changed during the build", [](FakeWorld& w) { w.wasm_other_smoke_input = true; },
         "src/worker/wasm/smoke-input.bin changed during the build"},
        {"the host node reports another version", [](FakeWorld& w) { w.wasm_wrong_host_version = true; },
         "host smoke test: version 'dc_worker 0', self-test 'OK'"},
        {"the host seconds per frame differ", [](FakeWorld& w) { w.wasm_host_spf_differs = true; },
         "seconds per frame of fcv1 differs: host 0.5, container 0.001"},
        {"the host shots differ", [](FakeWorld& w) { w.wasm_host_shots_differ = true; },
         "the fcv1 smoke shots of the host node differ from the container's"},
    };
    for (const Case& item : cases) {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        item.setup(world);
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_prebuild(wasm_options(synthetic)); });
        CHECK(message.find(item.fragment) == 0);
        CHECK(synthetic.snapshot() == before);  // nothing is installed
    }
}

TEST(wasm_worker_only_does_not_depend_on_the_host_machine) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.host_machine = "aarch64";  // only the native Linux entries need an x86-64 host
    CHECK_EQ(run_prebuild(wasm_options(synthetic)), 0);
    CHECK(fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm" / "manifest.json"));
}

/// --- the Linux hosts ---------------------------------------------------------------------------------------------------

namespace {

bool starts_with_text(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

bool starts_with_any(const std::string& text, const std::vector<std::string>& prefixes) {
    return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& prefix) { return starts_with_text(text, prefix); });
}

void edit_json_file(const fs::path& path, const std::function<void(Json&)>& change) {
    Json value = read_json(path);
    change(value);
    write_bytes(path, manifest_text(value));
}

PrebuildOptions linux_options(const SyntheticRoot& synthetic) {
    PrebuildOptions options;
    options.root = synthetic.root;
    return options;
}

// The number of `docker run` calls that build something (the ownership hand-over `chown` is not one).
std::size_t build_runs(const FakeWorld& world) {
    std::size_t runs = 0;
    for (const auto& call : world.docker_calls) runs += (call[0] == "run" && !contains(call, "chown")) ? 1 : 0;
    return runs;
}

// An aarch64 Linux host whose default image is an arm64 one.
void make_arm64_host(FakeWorld& world) {
    world.host_machine = "aarch64";
    world.architecture["dc_determinism-cpu:local"] = "arm64";
}

const char* const kArm64Ids[] = {"gcc", "clang", "gcc-nofma", "clang-nofma"};

// The arm64 data of a run of an aarch64 host or a container: worker, loaders, players, toolchain.
void check_arm64_manifest(const SyntheticRoot& synthetic, const Json& before, const std::string& image, const std::string& image_id) {
    Json manifest = synthetic.read();
    Json worker = manifest["binaries"]["linux-arm64"];
    CHECK_EQ(worker["path"], Json("prebuilt/worker/linux-arm64/dc_worker"));
    CHECK_EQ(worker["flags"], Json(linux_arm64_worker_flags()));
    CHECK_EQ(worker["image"], Json(image));
    CHECK_EQ(worker["image_id"], Json(image_id));
    CHECK_EQ(worker["sha256"].get<std::string>(), sha256_file(synthetic.root / "prebuilt" / "worker" / "linux-arm64" / "dc_worker"));
    CHECK((keys_of(manifest["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "linux-arm64"}));
    CHECK_EQ(manifest["binaries"]["linux-amd64"], before["binaries"]["linux-amd64"]);
    CHECK_EQ(manifest["binaries"]["windows-amd64"], before["binaries"]["windows-amd64"]);
    // the four ids of the players, before the carried wasm entries
    std::vector<std::string> player_keys = keys_of(manifest["players"]["binaries"]);
    for (const char* id : kArm64Ids) {
        for (const char* player : {"normal_dist", "nd2"}) {
            std::string key = std::string("linux-arm64-") + id + "/" + player;
            CHECK(contains(player_keys, key));
            const Json& entry = manifest["players"]["binaries"][key];
            CHECK_EQ(entry["platform"], Json("linux-arm64"));
            CHECK_EQ(entry["compiler_id"], Json(id));
            CHECK_EQ(entry["compiler"], Json(std::string(id).compare(0, 3, "gcc") == 0 ? "g++ (fake Ubuntu) 13.3.0" : "Ubuntu clang version fake"));
            CHECK_EQ(entry["flags"], Json(prebuilt_player_flags(std::string("linux-") + id, player)));
            CHECK_EQ(entry["image_id"], Json(image_id));
            fs::path file = synthetic.root / path_from_utf8(entry["path"].get<std::string>());
            CHECK_EQ(entry["path"], Json(std::string("prebuilt/players/linux-arm64-") + id + "/" + player + "/worker"));
            CHECK_EQ(sha256_file(file), entry["sha256"].get<std::string>());
        }
    }
    CHECK_EQ(manifest["players"]["binaries"]["wasm/nd2"], before["players"]["binaries"]["wasm/nd2"]);
    CHECK_EQ(manifest["players"]["binaries"]["windows-amd64-msvc/normal_dist"], before["players"]["binaries"]["windows-amd64-msvc/normal_dist"]);
    auto position = [&](const std::string& key) { return std::find(player_keys.begin(), player_keys.end(), key) - player_keys.begin(); };
    CHECK(position("linux-amd64-gcc/normal_dist") < position("linux-arm64-gcc/normal_dist"));
    CHECK(position("linux-arm64-clang-nofma/nd2") < position("wasm/nd2"));
    // the loaders of the four ids, with the schema of the platform and the fused multiply-add count of each library
    Json loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json");
    Json windows_loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    CHECK_EQ(loaders["schema"], Json("dc-prebuilt-linux-arm64-v1"));
    CHECK_EQ(loaders["image_id"], Json(image_id));
    CHECK_EQ(loaders["source"], windows_loaders["source"]);
    CHECK_EQ(loaders["dependencies"], windows_loaders["dependencies"]);
    CHECK((keys_of(loaders["libraries"]) == std::vector<std::string>{"gcc", "clang", "gcc-nofma", "clang-nofma"}));
    for (const char* id : kArm64Ids) {
        const Json& library = loaders["libraries"][id];
        bool nofma = std::string(id).find("nofma") != std::string::npos;
        CHECK_EQ(library["filename"], Json("libdigitalcurling_plugin_loader.so"));
        CHECK_EQ(library["sha256"].get<std::string>(),
                 sha256_file(synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / id / "libdigitalcurling_plugin_loader.so"));
        CHECK_EQ(library["build"]["fused_multiply_add_instructions"], Json(nofma ? 0 : 12));
        CHECK_EQ(library["build"].contains("flags_extra"), nofma);
        if (nofma) CHECK_EQ(library["build"]["flags_extra"], Json::parse(R"(["-ffp-contract=off"])"));
        CHECK_EQ(library["build"]["target"], Json("aarch64-linux-gnu"));
    }
    // toolchain.linux-arm64 is written by an arm64 host only
    CHECK_EQ(manifest["toolchain"]["linux-arm64"], Json::parse("{\"image\": \"" + image + "\", \"image_id\": \"" + image_id + "\"}"));
}

}  // namespace

TEST(an_x86_64_linux_host_builds_loaders_worker_players_and_wasm_and_writes_only_its_keys) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    Json before = synthetic.read();
    fs::path installed_worker = synthetic.root / "prebuilt" / "worker" / "linux-amd64" / "dc_worker";
    write_bytes(installed_worker, "old worker");
#ifndef _WIN32
    // A target that cannot be written in place (a file of another owner on a mounted drive) is replaced all the same.
    fs::permissions(installed_worker, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
#endif
    std::map<std::string, std::string> files = synthetic.snapshot();

    PrebuildOptions options = linux_options(synthetic);
    options.image = "other-image:1";
    CHECK_EQ(run_linux_prebuild(options), 0);
    CHECK_EQ(read_file(installed_worker), "linux dc_worker " + synthetic.identity.worker_sha256);

    // Only the keys of linux-amd64 and of wasm (nobody records another emsdk image) changed.
    for (const std::string& file : changed_files(files, synthetic.snapshot())) {
        CHECK(file == "manifest.json" || starts_with_any(file, {"worker/linux-amd64/", "worker/wasm/", "loaders/linux-amd64/", "players/linux-amd64-", "players/wasm/"}));
    }
    Json manifest = synthetic.read();
    CHECK((keys_of(manifest) == std::vector<std::string>{"schema", "source", "binaries", "players", "toolchain", "dc_determinism"}));
    CHECK_EQ(manifest["source"], before["source"]);
    CHECK_EQ(manifest["players"]["source"], before["players"]["source"]);
    CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
    CHECK_EQ(manifest["binaries"]["windows-amd64"], before["binaries"]["windows-amd64"]);
    Json worker = manifest["binaries"]["linux-amd64"];
    CHECK_EQ(worker["sha256"].get<std::string>(), sha256_file(installed_worker));
    CHECK_EQ(worker["image"], Json("other-image:1"));
    CHECK_EQ(worker["image_id"], Json("sha256:id-of-other-image:1"));
    CHECK_EQ(worker["flags"], Json(linux_worker_flags()));
    const Json& players = manifest["players"]["binaries"];
    CHECK_EQ(players["windows-amd64-msvc/normal_dist"], before["players"]["binaries"]["windows-amd64-msvc/normal_dist"]);
    CHECK((keys_of(players) == std::vector<std::string>{"windows-amd64-msvc/normal_dist", "linux-amd64-gcc/normal_dist", "linux-amd64-gcc/nd2",
                                                        "linux-amd64-clang/normal_dist", "linux-amd64-clang/nd2", "wasm/normal_dist", "wasm/nd2"}));
    CHECK_EQ(players["linux-amd64-gcc/normal_dist"]["sha256"].get<std::string>(),
             sha256_file(synthetic.root / "prebuilt" / "players" / "linux-amd64-gcc" / "normal_dist" / "worker"));
    CHECK_EQ(players["linux-amd64-clang/nd2"]["compiler"], Json("Ubuntu clang version fake"));
    CHECK_EQ(players["linux-amd64-clang/normal_dist"]["image"], Json("other-image:1"));
    CHECK_EQ(players["wasm/nd2"]["image"], Json("emscripten/emsdk:6.0.9"));
    CHECK((keys_of(manifest["toolchain"]) == std::vector<std::string>{"linux-amd64", "wasm"}));
    CHECK_EQ(manifest["toolchain"]["linux-amd64"]["image_id"], Json("sha256:id-of-other-image:1"));

    // The loaders of the host's platform, in the layout of the Windows host's linux-amd64 part.
    Json loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "linux-amd64" / "manifest.json");
    CHECK_EQ(loaders["schema"], Json("dc-prebuilt-linux-amd64-v1"));
    CHECK_EQ(loaders["image_id"], Json("sha256:id-of-other-image:1"));
    CHECK((keys_of(loaders["libraries"]) == std::vector<std::string>{"gcc", "clang"}));
    CHECK_EQ(loaders["libraries"]["gcc"]["build"]["compiler_version"], Json("g++ (fake Ubuntu) 13.3.0"));
    CHECK(!loaders["libraries"]["gcc"]["build"].contains("fused_multiply_add_instructions"));
    CHECK((keys_of(loaders) == std::vector<std::string>{"schema", "image_id", "source", "dependencies", "libraries"}));
    Json windows_loader_manifest = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    CHECK_EQ(loaders["source"], windows_loader_manifest["source"]);
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "loaders" / "linux-arm64"));
    // The wasm bundle is the host's too (no other image is recorded).
    CHECK(fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm" / "manifest.json"));

    // Docker: the worker, the two loaders, the wasm worker, the linux players and the wasm players; nothing windows.
    CHECK(world.builds.empty() && world.cmake_calls.empty() && world.probed.empty());
    std::vector<std::string> order;
    for (const auto& call : world.docker_calls) {
        if (call[0] != "run" || contains(call, "chown")) continue;
        order.push_back(call.back() == "/repo/src/worker/wasm/build-wasm-worker.sh" ? "wasm-worker" : "script");
        bool wasm = contains(call, "emscripten/emsdk:6.0.9");
        CHECK(wasm || (contains(call, "-u") && contains(call, "1000:1000")));  // the outputs belong to the calling user
    }
    CHECK((order == std::vector<std::string>{"script", "script", "script", "wasm-worker", "script", "script"}));
    CHECK((world.script_environments[1] == std::vector<std::string>{"COMP=gcc"} && world.script_environments[2] == std::vector<std::string>{"COMP=clang"}));
}

TEST(linux_host_worker_only_leaves_the_players_the_loaders_and_wasm_alone) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    Json before = synthetic.read();
    PrebuildOptions options = linux_options(synthetic);
    options.worker_only = true;
    CHECK_EQ(run_linux_prebuild(options), 0);
    Json manifest = synthetic.read();
    CHECK_EQ(manifest["players"], before["players"]);
    CHECK(manifest["binaries"]["linux-amd64"]["sha256"] != before["binaries"]["linux-amd64"]["sha256"]);
    CHECK((keys_of(manifest["toolchain"]) == std::vector<std::string>{"linux-amd64"}));
    CHECK_EQ(build_runs(world), static_cast<std::size_t>(1));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "loaders" / "linux-amd64" / "gcc"));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "wasm"));
}

TEST(linux_host_without_the_default_image_stops_instead_of_falling_back) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.present = {"other-image:1", "emscripten/emsdk:6.0.9"};  // dc_determinism-cpu:local is missing
    PrebuildOptions options = linux_options(synthetic);
    options.worker_only = true;
    std::string message = error_of([&] { run_linux_prebuild(options); });
    CHECK(message.find("Docker image dc_determinism-cpu:local not found") != std::string::npos);
    for (const auto& call : world.docker_calls) CHECK(call[0] != "run");  // nothing was built
}

TEST(a_host_that_is_not_the_windows_host_refuses_a_changed_source_identity) {
    const std::string advice = "\nA full rebuild of all platforms is required: run `dc_determinism prebuild` on a Windows host.";
    for (const char* machine : {"x86_64", "aarch64"}) {
        {
            // worker.cpp differs from the manifest
            SyntheticRoot synthetic;
            FakeWorld world(synthetic.identity.worker_sha256);
            if (std::string(machine) == "aarch64") make_arm64_host(world);
            write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int changed;\n");
            std::string before = synthetic.text();
            std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
            CHECK(message.find("worker.cpp changed (sha256 ") == 0);
            CHECK(ends_with(message, advice));
            CHECK_EQ(synthetic.text(), before);
            CHECK(world.docker_calls.empty());
        }
        {
            // the player worker differs: refused for the players, accepted for a worker-only rebuild
            SyntheticRoot synthetic;
            FakeWorld world(synthetic.identity.worker_sha256);
            if (std::string(machine) == "aarch64") make_arm64_host(world);
            write_bytes(synthetic.root / "src" / "worker" / "player.cpp", "int changed;\n");
            PrebuildOptions options = linux_options(synthetic);
            CHECK(error_of([&] { run_linux_prebuild(options); }).find("src/worker/player.cpp changed") == 0);
            options.worker_only = true;
            CHECK(error_of([&] { run_linux_prebuild(options); }).empty());
        }
    }
    {
        // a vendored tree that does not match its recorded hash
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        write_bytes(synthetic.root / "src" / "vendor" / "nlohmann-json" / "stray.txt", "x");
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("vendored source verification failed") == 0);
        CHECK(ends_with(message, advice));
    }
    {
        // the dependency pins of the manifest differ
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        Json manifest = synthetic.read();
        manifest["players"]["source"]["dependencies"]["fpm"]["commit"] = std::string(40, '0');
        synthetic.write_manifest(manifest);
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("fpm HEAD differs from the pin " + std::string(40, '0')) == 0);
    }
    {
        // no manifest at all
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        fs::remove(synthetic.root / "prebuilt" / "manifest.json");
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("prebuilt/manifest.json is missing.") == 0);
        CHECK(ends_with(message, advice));
    }
    // The loader source and dependencies are compared with the existing loader manifests (new): each platform's file.
    for (const char* platform : {"windows-amd64", "linux-amd64"}) {
        for (const char* member : {"source", "dependencies"}) {
            SyntheticRoot synthetic;
            FakeWorld world(synthetic.identity.worker_sha256);
            edit_json_file(synthetic.root / "prebuilt" / "loaders" / platform / "manifest.json", [&](Json& value) {
                if (std::string(member) == "source") value["source"]["revision"] = "other";
                else value["dependencies"]["xxhash"] = "other";
            });
            std::map<std::string, std::string> before = synthetic.snapshot();
            std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
            CHECK(message.find(std::string("the loader ") + member + (std::string(member) == "source" ? " differs from prebuilt/loaders/" : " differ from prebuilt/loaders/") +
                               platform + "/manifest.json") == 0);
            CHECK(ends_with(message, advice));
            CHECK(synthetic.snapshot() == before);
            CHECK(world.docker_calls.empty());
            // the worker-only mode does not touch the loaders: it is not refused for them
            PrebuildOptions options = linux_options(synthetic);
            options.worker_only = true;
            CHECK(error_of([&] { run_linux_prebuild(options); }).empty());
        }
    }
    // The public predicate: none for the sources of the repository, the first difference for the others.
    SyntheticRoot synthetic;
    CHECK(!loaders_mismatch(synthetic.root));
    edit_json_file(synthetic.root / "prebuilt" / "loaders" / "linux-amd64" / "manifest.json", [](Json& value) { value["dependencies"]["box2d"] = "other"; });
    std::optional<std::string> mismatch = loaders_mismatch(synthetic.root);
    CHECK(mismatch && mismatch->find("the loader dependencies differ from prebuilt/loaders/linux-amd64/manifest.json") == 0);
}

TEST(prebuild_needs_the_docker_cli) {
    // Only where the real Docker CLI is absent (for example in the dc_determinism container): never start a real container here.
    if (find_on_path("docker")) return;
    SyntheticRoot synthetic;
    PrebuildOptions options = linux_options(synthetic);
    options.worker_only = true;
    CHECK(error_of([&] { run_linux_prebuild(options); }).find("Docker CLI") != std::string::npos);
    CHECK(error_of([&] { run_windows_prebuild(options); }).find("Docker CLI") != std::string::npos);
    PrebuildOptions wasm;
    wasm.root = synthetic.root;
    wasm.wasm_worker_only = true;
    CHECK(error_of([&] { run_prebuild(wasm); }).find("Docker CLI") != std::string::npos);
}

TEST(an_aarch64_linux_host_builds_the_four_arm64_ids_and_records_the_arm64_toolchain) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    make_arm64_host(world);
    Json before = synthetic.read();
    std::map<std::string, std::string> files = synthetic.snapshot();

    CHECK_EQ(run_linux_prebuild(linux_options(synthetic)), 0);
    check_arm64_manifest(synthetic, before, "dc_determinism-cpu:local", "sha256:id-of-dc_determinism-cpu:local");
    Json manifest = synthetic.read();
    CHECK(!manifest["toolchain"].contains("linux-amd64"));  // the other hosts' facts are not written here
    CHECK(!manifest["toolchain"].contains("wasm"));         // the amd64 Emscripten image cannot build here: wasm is carried
    CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
    for (const std::string& file : changed_files(files, synthetic.snapshot())) {
        CHECK(file == "manifest.json" || starts_with_any(file, {"worker/linux-arm64/", "loaders/linux-arm64/", "players/linux-arm64-"}));
    }
    // One run per script: the worker, the four loaders, the players; no wasm build.
    CHECK_EQ(build_runs(world), static_cast<std::size_t>(1 + 4 + 1));
    CHECK(world.wasm_builds.empty());
    CHECK(world.scripts[0].find("-ffp-contract=off") != std::string::npos);  // the worker
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(world.script_environments[1 + i] == std::vector<std::string>{std::string("COMP=") + kArm64Ids[i]});
        CHECK(world.scripts[1 + i].find("fma.txt") != std::string::npos);
    }
    CHECK(world.scripts[5].find("for comp in gcc clang gcc-nofma clang-nofma; do") != std::string::npos);
    // "arm64" (the name macOS gives) is an aarch64 host too
    world.host_machine = "arm64";
    CHECK_EQ(run_linux_prebuild(linux_options(synthetic)), 0);
}

TEST(the_fused_multiply_add_gate_of_the_nofma_loaders_stops_the_run_and_the_counts_are_recorded) {
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_arm64_host(world);
        world.fma_counts["gcc-nofma"] = 3;
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("the gcc-nofma loader contains 3 fused multiply-add instructions") == 0);
        CHECK(synthetic.snapshot() == before);  // nothing was installed
    }
    {
        // a plain id may contain them: the count is recorded, not gated
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_arm64_host(world);
        world.fma_counts["gcc"] = 40;
        world.fma_counts["clang"] = 0;
        CHECK_EQ(run_linux_prebuild(linux_options(synthetic)), 0);
        Json loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json");
        CHECK_EQ(loaders["libraries"]["gcc"]["build"]["fused_multiply_add_instructions"], Json(40));
        CHECK_EQ(loaders["libraries"]["clang"]["build"]["fused_multiply_add_instructions"], Json(0));
    }
    {
        // a count that is not a number is not an answer
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_arm64_host(world);
        world.fma_counts["gcc"] = -1;
        CHECK(error_of([&] { run_linux_prebuild(linux_options(synthetic)); }).find("the fused multiply-add count of the gcc loader is not a number") == 0);
    }
    {
        // no objdump in the image: a clear error from the build script (the amd64 script never needs it)
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_arm64_host(world);
        world.objdump_missing = true;
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("docker linux gcc loader failed (exit code 1):\nobjdump was not found in the image (binutils)") == 0);
    }
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.objdump_missing = true;  // x86-64: no gate, no objdump
        CHECK_EQ(run_linux_prebuild(linux_options(synthetic)), 0);
    }
}

TEST(a_docker_image_of_another_architecture_than_the_host_is_refused_before_building) {
    struct Case {
        const char* what;
        const char* machine;       // "" : the Windows host
        const char* image;
        const char* image_arch;
        const char* message;
    };
    const Case cases[] = {
        {"amd64 Linux host, arm64 image", "x86_64", "dc_determinism-cpu:local-arm64", "arm64",
         "the Docker image dc_determinism-cpu:local-arm64 is arm64, but `dc_determinism prebuild` on this host builds linux-amd64 and needs an amd64 image"},
        {"arm64 Linux host, amd64 image", "aarch64", "dc_determinism-cpu:local", "amd64",
         "the Docker image dc_determinism-cpu:local is amd64, but `dc_determinism prebuild` on this host builds linux-arm64 and needs an arm64 image"},
        {"Windows host, arm64 image", "", "dc_determinism-cpu:local-arm64", "arm64",
         "the Docker image dc_determinism-cpu:local-arm64 is arm64, but `dc_determinism prebuild` on this host builds linux-amd64 and needs an amd64 image"},
    };
    for (const Case& item : cases) {
        for (bool worker_only : {false, true}) {
            SyntheticRoot synthetic;
            synthetic.add_arm64();
            FakeWorld world(synthetic.identity.worker_sha256);
            if (std::string(item.machine).size()) world.host_machine = item.machine;
            world.present.insert(item.image);
            world.architecture[item.image] = item.image_arch;
            if (std::string(item.image) == "dc_determinism-cpu:local") world.present.insert("dc_determinism-cpu:local");
            std::map<std::string, std::string> before = synthetic.snapshot();
            PrebuildOptions options = linux_options(synthetic);
            options.image = item.image;
            options.worker_only = worker_only;
            std::string message = error_of([&] {
                if (std::string(item.machine).empty()) run_windows_prebuild(options);
                else run_linux_prebuild(options);
            });
            CHECK(message.find(item.message) == 0);
            CHECK(message.find("(nothing is emulated)") != std::string::npos);
            CHECK(synthetic.snapshot() == before);
            CHECK(world.builds.empty() && world.probed.empty());  // refused before any build
            CHECK_EQ(build_runs(world), static_cast<std::size_t>(0));
        }
    }
    {
        // the default image is checked too, and an Emscripten image of another architecture named by --emsdk-image
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.architecture["dc_determinism-cpu:local"] = "arm64";
        PrebuildOptions options = linux_options(synthetic);
        options.worker_only = true;
        CHECK(error_of([&] { run_windows_prebuild(options); }).find("dc_determinism-cpu:local is arm64") != std::string::npos);
    }
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_arm64_host(world);  // the Emscripten image stays amd64
        PrebuildOptions options = linux_options(synthetic);
        options.emsdk_image_explicit = true;
        std::string message = error_of([&] { run_linux_prebuild(options); });
        CHECK(message.find("the Docker image emscripten/emsdk:6.0.9 is amd64, but `dc_determinism prebuild` on this host builds linux-arm64") == 0);
        CHECK_EQ(build_runs(world), static_cast<std::size_t>(0));
    }
}

TEST(a_linux_host_that_is_neither_x86_64_nor_aarch64_is_refused_before_anything_else) {
    for (const char* machine : {"riscv64", "i686", ""}) {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        world.host_machine = machine;
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_linux_prebuild(linux_options(synthetic)); });
        CHECK(message.find("supports x86_64 and aarch64 machines (this host is ") != std::string::npos);
        CHECK(message.find(std::string("prebuild-") + "arm64") == std::string::npos);  // the retired script is not advised
        CHECK(synthetic.snapshot() == before);
        CHECK(world.docker_calls.empty());
    }
}

// --- the Windows host and the linux-arm64 data it carries ----------------------------------------------------------------

namespace {

// The mode under test: Windows full rebuild, Windows `--worker-only`, or the x86-64 Linux host.
enum class Mode { WindowsFull, WindowsWorkerOnly, Linux };

void run_mode(Mode mode, PrebuildOptions options) {
    options.worker_only = options.worker_only || mode == Mode::WindowsWorkerOnly;
    if (mode == Mode::Linux) {
        run_linux_prebuild(options);
    } else {
        run_windows_prebuild(options);
    }
}

const Mode kModes[] = {Mode::WindowsFull, Mode::WindowsWorkerOnly, Mode::Linux};

std::vector<std::string> arm64_files(const SyntheticRoot& synthetic) {
    std::vector<std::string> names;
    for (const auto& item : synthetic.snapshot()) {
        if (item.first.find("linux-arm64") != std::string::npos) names.push_back(item.first);
    }
    return names;
}

}  // namespace

TEST(worker_and_player_mismatch_are_separate_predicates) {
    SyntheticRoot synthetic;
    Json manifest = synthetic.manifest();
    SourceIdentity worker = synthetic.identity;
    worker.worker_sha256 = std::string(64, 'a');
    CHECK(worker_mismatch(manifest, worker).has_value());
    CHECK(!players_mismatch(manifest, worker));
    SourceIdentity player = synthetic.identity;
    player.player_worker_sha256 = std::string(64, 'b');
    CHECK(!worker_mismatch(manifest, player));
    CHECK(players_mismatch(manifest, player).has_value());
    // identity_mismatch is the two in order.
    CHECK(identity_mismatch(manifest, worker, true) == worker_mismatch(manifest, worker));
    CHECK(identity_mismatch(manifest, player, true) == players_mismatch(manifest, player));
    CHECK(!identity_mismatch(manifest, player, false));
}

TEST(every_mode_carries_the_linux_arm64_files_and_entries_that_match_the_sources) {
    for (Mode mode : kModes) {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        FakeWorld world(synthetic.identity.worker_sha256);
        Json before = synthetic.read();
        std::map<std::string, std::string> files = synthetic.snapshot();
        write_bytes(synthetic.root / "prebuilt" / "players" / "stale" / "left-over.txt", "old player");

        run_mode(mode, linux_options(synthetic));

        Json manifest = synthetic.read();
        CHECK_EQ(manifest["binaries"]["linux-arm64"], before["binaries"]["linux-arm64"]);
        CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
        CHECK_EQ(manifest["toolchain"]["linux-arm64"], before["toolchain"]["linux-arm64"]);
        for (const char* key : {"linux-arm64-gcc/normal_dist", "linux-arm64-gcc-nofma/nd2"}) {
            CHECK_EQ(manifest["players"]["binaries"][key], before["players"]["binaries"][key]);
        }
        CHECK((keys_of(manifest["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "linux-arm64"}));
        // The arm64 files (worker, two players, the loader manifest and its library) are byte for byte the same.
        std::map<std::string, std::string> after = synthetic.snapshot();
        for (const auto& item : files) {
            if (item.first.find("linux-arm64") != std::string::npos) CHECK_EQ(after[item.first], item.second);
        }
        CHECK_EQ(arm64_files(synthetic).size(), static_cast<std::size_t>(5));
        if (mode == Mode::WindowsFull) {
            // The rebuilt players replace every other player directory, the arm64 ones stay and sit before the wasm entries.
            CHECK(!fs::exists(synthetic.root / "prebuilt" / "players" / "stale"));
            std::vector<std::string> players = keys_of(manifest["players"]["binaries"]);
            auto arm64 = std::find(players.begin(), players.end(), "linux-arm64-gcc/normal_dist");
            CHECK(arm64 != players.end() && *(arm64 + 1) == "linux-arm64-gcc-nofma/nd2" && *(arm64 + 2) == "wasm/normal_dist");
        } else if (mode == Mode::Linux) {
            CHECK(fs::exists(synthetic.root / "prebuilt" / "players" / "stale"));  // another host's directories are not touched
            CHECK((keys_of(manifest["players"]["binaries"]).size() == keys_of(before["players"]["binaries"]).size() + 3));
        } else {
            CHECK_EQ(manifest["players"], before["players"]);
        }
    }
}

TEST(a_windows_host_removes_the_linux_arm64_data_that_the_sources_made_stale_after_everything_else) {
    // worker.cpp changed (the full rebuild): the arm64 worker, its directory and toolchain.linux-arm64 go; players and loaders stay
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int changed_worker;\n");
        FakeWorld world(sha256_lf_file(synthetic.root / "src" / "worker" / "worker.cpp"));
        Json before = synthetic.read();
        bool files_present_at_merge = false;
        bool entry_gone_at_merge = false;
        prebuild_hooks().adjust_merged_manifest = [&](Json& merged) {
            files_present_at_merge = fs::exists(synthetic.root / "prebuilt" / "worker" / "linux-arm64" / "dc_worker");
            entry_gone_at_merge = !merged["binaries"].contains("linux-arm64") && !merged["toolchain"].contains("linux-arm64");
        };
        CHECK_EQ(run_windows_prebuild(linux_options(synthetic)), 0);
        CHECK(files_present_at_merge && entry_gone_at_merge);  // the files are removed after the manifest was written
        Json manifest = synthetic.read();
        CHECK((keys_of(manifest["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64"}));
        CHECK(!manifest["toolchain"].contains("linux-arm64"));
        CHECK(manifest["players"]["binaries"].contains("linux-arm64-gcc/normal_dist"));
        CHECK_EQ(manifest["dc_determinism"], before["dc_determinism"]);
        CHECK(!fs::exists(synthetic.root / "prebuilt" / "worker" / "linux-arm64"));
        CHECK(arm64_files(synthetic).size() == 4);  // two players, the loader manifest and library
    }
    // the player source changed: the arm64 players go, the worker stays
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        write_bytes(synthetic.root / "src" / "worker" / "player.cpp", "int changed_player_worker;\n");
        FakeWorld world(synthetic.identity.worker_sha256);
        Json before = synthetic.read();
        CHECK_EQ(run_windows_prebuild(linux_options(synthetic)), 0);
        Json manifest = synthetic.read();
        CHECK_EQ(manifest["binaries"]["linux-arm64"], before["binaries"]["linux-arm64"]);
        for (const std::string& key : keys_of(manifest["players"]["binaries"])) CHECK(key.compare(0, 12, "linux-arm64-") != 0);
        CHECK(!manifest["toolchain"].contains("linux-arm64"));
        for (const std::string& file : arm64_files(synthetic)) CHECK(file.find("players/") == std::string::npos);
        CHECK(fs::exists(synthetic.root / "prebuilt" / "worker" / "linux-arm64" / "dc_worker"));
    }
    // the loader source or dependencies changed: the arm64 loaders go
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        edit_json_file(synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json", [](Json& value) { value["source"]["revision"] = "other"; });
        FakeWorld world(synthetic.identity.worker_sha256);
        Json before = synthetic.read();
        CHECK_EQ(run_windows_prebuild(linux_options(synthetic)), 0);
        Json manifest = synthetic.read();
        CHECK_EQ(manifest["binaries"]["linux-arm64"], before["binaries"]["linux-arm64"]);
        CHECK(manifest["players"]["binaries"].contains("linux-arm64-gcc/normal_dist"));
        CHECK(!fs::exists(synthetic.root / "prebuilt" / "loaders" / "linux-arm64"));
        CHECK(!manifest["toolchain"].contains("linux-arm64"));
    }
    // everything stale at once
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int changed_worker;\n");
        write_bytes(synthetic.root / "src" / "worker" / "player.cpp", "int changed_player_worker;\n");
        edit_json_file(synthetic.root / "prebuilt" / "loaders" / "linux-arm64" / "manifest.json", [](Json& value) { value["dependencies"]["box2d"] = "other"; });
        FakeWorld world(sha256_lf_file(synthetic.root / "src" / "worker" / "worker.cpp"));
        CHECK_EQ(run_windows_prebuild(linux_options(synthetic)), 0);
        CHECK(arm64_files(synthetic).empty());
        Json manifest = synthetic.read();
        CHECK(!manifest["binaries"].contains("linux-arm64") && !manifest["toolchain"].contains("linux-arm64"));
        CHECK((keys_of(manifest["toolchain"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "wasm"}));
    }
    // the worker-only mode compares the worker source alone: it does not rebuild the players or the loaders
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        write_bytes(synthetic.root / "src" / "worker" / "player.cpp", "int changed_player_worker;\n");
        FakeWorld world(synthetic.identity.worker_sha256);
        PrebuildOptions options = linux_options(synthetic);
        options.worker_only = true;
        CHECK_EQ(run_windows_prebuild(options), 0);
        CHECK(synthetic.read()["players"]["binaries"].contains("linux-arm64-gcc/normal_dist"));
        CHECK_EQ(arm64_files(synthetic).size(), static_cast<std::size_t>(5));
    }
    // a failed run removes nothing
    {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int changed_worker;\n");
        FakeWorld world(sha256_lf_file(synthetic.root / "src" / "worker" / "worker.cpp"));
        world.bad_restore = true;
        std::map<std::string, std::string> before = synthetic.snapshot();
        CHECK(!error_of([&] { run_windows_prebuild(linux_options(synthetic)); }).empty());
        CHECK(synthetic.snapshot() == before);
    }
}

TEST(an_unknown_member_of_the_manifest_is_carried_while_the_carried_member_check_guards_the_tool_section) {
    // `extra` is a member this version does not know; prebuild keeps it, `dc_determinism` is guarded.
    for (Mode mode : kModes) {
        SyntheticRoot synthetic;
        synthetic.add_arm64();
        Json value = synthetic.read();
        value["extra"] = Json::parse(R"({"source": {"path": "src/extra"}, "binaries": {"linux-amd64": {"sha256": "extra"}}})");
        synthetic.write_manifest(value);
        FakeWorld world(synthetic.identity.worker_sha256);
        run_mode(mode, linux_options(synthetic));
        Json manifest = synthetic.read();
        CHECK_EQ(manifest["extra"], value["extra"]);
        CHECK_EQ(manifest["dc_determinism"], value["dc_determinism"]);
        CHECK(keys_of(manifest).back() == "extra");
    }
}

TEST(the_carried_member_check_stops_a_sabotaged_merge_before_anything_is_written) {
    struct Case {
        const char* name;
        std::function<void(Json&)> sabotage;
        const char* fragment;
    };
    std::vector<Case> cases = {
        {"arm64 worker lost", [](Json& merged) { merged["binaries"].erase("linux-arm64"); }, "binaries.linux-arm64 was not kept"},
        {"arm64 player lost", [](Json& merged) { merged["players"]["binaries"].erase("linux-arm64-gcc/normal_dist"); },
         "players.binaries[linux-arm64-gcc/normal_dist] was not kept"},
        {"arm64 player changed", [](Json& merged) { merged["players"]["binaries"]["linux-arm64-gcc-nofma/nd2"]["sha256"] = "x"; },
         "players.binaries[linux-arm64-gcc-nofma/nd2] was not kept"},
        {"arm64 toolchain lost", [](Json& merged) { merged["toolchain"].erase("linux-arm64"); }, "toolchain.linux-arm64 was not kept"},
        {"dc_determinism section changed", [](Json& merged) { merged["dc_determinism"]["binaries"]["linux-amd64"]["sha256"] = "changed"; },
         "the dc_determinism section changed"},
        {"dc_determinism section lost", [](Json& merged) { merged.erase("dc_determinism"); }, "the dc_determinism section changed"},
        {"an unknown member changed", [](Json& merged) { merged["extra"]["carried"] = false; }, "the extra section changed"},
        {"an arm64 player appears", [](Json& merged) { merged["players"]["binaries"]["linux-arm64-clang/nd2"] = Json::object(); },
         "players.binaries[linux-arm64-clang/nd2] appeared"},
        {"a binary appears", [](Json& merged) { merged["binaries"]["macos-arm64"] = Json::object(); }, "binaries.macos-arm64 appeared"},
    };
    for (const Case& item : cases) {
        for (Mode mode : kModes) {
            SyntheticRoot synthetic;
            synthetic.add_arm64();
            Json value = synthetic.read();
            value["extra"] = Json::parse(R"({"carried": true})");
            synthetic.write_manifest(value);
            FakeWorld world(synthetic.identity.worker_sha256);
            prebuild_hooks().adjust_merged_manifest = item.sabotage;
            std::map<std::string, std::string> before = synthetic.snapshot();
            std::string message = error_of([&] { run_mode(mode, linux_options(synthetic)); });
            CHECK(message.find("internal check before writing the manifest failed: ") == 0);
            CHECK(message.find(item.fragment) != std::string::npos);
            CHECK(synthetic.snapshot() == before);  // the builds ran in scratch; prebuilt/ is untouched
        }
    }
}

TEST(a_manifest_that_does_not_read_back_as_written_is_reported) {
    // The manifest text is checked after writing: a merged manifest the writer changes would not read back equal.
    SyntheticRoot synthetic;
    synthetic.add_arm64();
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions options = linux_options(synthetic);
    options.worker_only = true;
    CHECK(error_of([&] { run_windows_prebuild(options); }).empty());
    Json written = synthetic.read();
    CHECK_EQ(manifest_text(written), synthetic.text());
}

// --- the in-container executor -------------------------------------------------------------------------------------------

namespace {

// The options of the wrapper's run inside the arm64 container (`dc_determinism.sh prebuild` on a Mac).
PrebuildOptions container_options(const SyntheticRoot& synthetic) {
    PrebuildOptions options;
    options.root = synthetic.root;
    options.image = "dc_determinism-cpu:local-arm64";
    options.image_id = "sha256:id-of-dc_determinism-cpu:local-arm64";
    options.build_user = "501:20";
    return options;
}

void make_container(FakeWorld& world) {
    world.host_machine = "aarch64";
    world.in_container = true;
    prebuild_hooks().windows_host = [] { return false; };  // the container is a Linux host, whatever runs the test
}

}  // namespace

TEST(inside_a_container_prebuild_needs_image_id_and_outside_one_it_refuses_the_hidden_options) {
    {
        // inside a container without --image-id: no Docker CLI there, say so (nothing runs)
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_container(world);
        PrebuildOptions options = linux_options(synthetic);
        std::string message = error_of([&] { run_prebuild(options); });
        CHECK(message.find("`prebuild` needs the Docker CLI and cannot run inside a container without --image-id") == 0);
        CHECK(world.docker_calls.empty());
        options.image_id = "sha256:1";  // --image-id without --image
        CHECK(error_of([&] { run_prebuild(options); }).find("--image-id needs --image") == 0);
        options.image = "dc_determinism-cpu:local-arm64";
        options.build_user = "root";
        CHECK(error_of([&] { run_prebuild(options); }).find("--build-user must be UID:GID") == 0);
        options.build_user = "1000:";
        CHECK(error_of([&] { run_prebuild(options); }).find("--build-user must be UID:GID") == 0);
        CHECK(world.docker_calls.empty());
    }
    {
        // outside a container the options that select the in-container executor are refused
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        for (const char* which : {"image_id", "build_user"}) {
            PrebuildOptions options = linux_options(synthetic);
            (std::string(which) == "image_id" ? options.image_id : options.build_user) = "x";
            for (const char* mode : {"windows", "linux"}) {
                std::string message = error_of([&] {
                    if (std::string(mode) == "windows") run_windows_prebuild(options);
                    else run_linux_prebuild(options);
                });
                CHECK(message.find("--image-id and --build-user are accepted only inside a container") == 0);
            }
        }
        CHECK(world.docker_calls.empty());
    }
    {
        // --wasm-worker-only needs Docker and is refused in a container, with or without --image-id
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_container(world);
        PrebuildOptions options = container_options(synthetic);
        options.wasm_worker_only = true;
        options.image.clear();
        std::string message = error_of([&] { run_prebuild(options); });
        CHECK(message.find("--wasm-worker-only needs the Docker CLI and cannot run inside a container") == 0);
        options.image_id.clear();
        options.build_user.clear();
        CHECK(error_of([&] { run_prebuild(options); }).find("--wasm-worker-only needs the Docker CLI and cannot run inside a container") == 0);
        CHECK(world.docker_calls.empty());
    }
    {
        // setpriv is a preflight: a clear error before anything is copied or run
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_container(world);
        world.has_setpriv = false;
        std::string message = error_of([&] { run_prebuild(container_options(synthetic)); });
        CHECK(message.find("setpriv was not found in this container (util-linux)") == 0);
        CHECK(!fs::exists(world.container_dir.path() / "src") && world.container_commands.empty());
        // without a build user nothing needs it
        PrebuildOptions no_user = container_options(synthetic);
        no_user.build_user.clear();
        CHECK(error_of([&] { run_prebuild(no_user); }).empty());
        CHECK(world.container_commands[0][2 + 0] != "setpriv");
        CHECK(!contains(world.container_commands[0], "setpriv"));
    }
}

TEST(the_in_container_executor_runs_the_same_scripts_with_the_same_environment_and_mounts_as_docker) {
    // The Docker executor (an aarch64 Linux host) and the in-container executor (the Mac) build the same repository.
    SyntheticRoot docker_root;
    SyntheticRoot container_root;
    FakeWorld docker_world(docker_root.identity.worker_sha256);
    make_arm64_host(docker_world);
    PrebuildOptions docker_options = linux_options(docker_root);
    docker_options.image = "dc_determinism-cpu:local-arm64";
    docker_world.present.insert("dc_determinism-cpu:local-arm64");
    docker_world.architecture["dc_determinism-cpu:local-arm64"] = "arm64";
    CHECK_EQ(run_linux_prebuild(docker_options), 0);
    std::vector<std::string> docker_scripts = docker_world.scripts;
    std::vector<std::vector<std::string>> docker_environments = docker_world.script_environments;
    std::vector<std::vector<std::string>> docker_mounts = docker_world.script_mounts;
    std::string docker_manifest = docker_root.text();
    std::map<std::string, std::string> docker_files = docker_root.snapshot();
    CHECK_EQ(docker_scripts.size(), static_cast<std::size_t>(6));

    {
        FakeWorld world(container_root.identity.worker_sha256);
        make_container(world);
        world.present.clear();  // Docker is not there: any call would fail
        CHECK_EQ(run_prebuild(container_options(container_root)), 0);
        // the same scripts in the same order, the same variables, the same mounts
        CHECK(world.scripts == docker_scripts);
        CHECK(world.script_environments == docker_environments);
        CHECK(world.script_mounts == docker_mounts);
        CHECK(!world.script_mounts.empty() && contains(world.script_mounts[0], "src") && contains(world.script_mounts[0], "out") &&
              contains(world.script_mounts[0], "build.sh"));
        CHECK(contains(world.script_mounts[1], "deps_box2d-patched") && contains(world.script_mounts[1], "deps_xxhash"));
        CHECK(contains(world.script_mounts[5], "player") && contains(world.script_mounts[5], "deps_fpm_include"));
        // no Docker call at all (the image is the container's own), the same bytes and the same manifest
        CHECK(world.docker_calls.empty());
        CHECK(container_root.text() == docker_manifest);
        CHECK(container_root.snapshot() == docker_files);
        check_arm64_manifest(container_root, container_root.manifest(), "dc_determinism-cpu:local-arm64", "sha256:id-of-dc_determinism-cpu:local-arm64");
    }
}

TEST(the_in_container_script_runs_under_env_i_as_the_build_user_with_the_images_environment) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    make_container(world);
    CHECK_EQ(run_prebuild(container_options(synthetic)), 0);
    CHECK_EQ(world.container_commands.size(), static_cast<std::size_t>(6));
    // worker: PATH and the image's ENV (the container environment without the variables the wrapper added: HOME, HOSTNAME,
    // TERM, DC_DETERMINISM_REPO, PYTHONDONTWRITEBYTECODE), the variable of the Docker executor, HOME=/ and HOSTNAME as
    // `docker run -u` sets them, setpriv with the numeric ids, bash /build.sh
    const std::vector<std::string> expected = {"env", "-i", "PATH=/usr/local/bin:/usr/bin", "DEBIAN_FRONTEND=noninteractive",
                                               "SOURCE_SHA=" + synthetic.identity.worker_sha256, "HOME=/", "HOSTNAME=abc123def456",
                                               "setpriv", "--reuid=501", "--regid=20", "--clear-groups", "bash", "/build.sh"};
    CHECK(world.container_commands[0] == expected);
    for (const auto& command : world.container_commands) {
        CHECK(!contains(command, "TERM=xterm") && !contains(command, "HOME=/root") && !contains(command, "PYTHONDONTWRITEBYTECODE=1"));
        CHECK(!contains(command, "DC_DETERMINISM_REPO=/w/dc_determinism") && !contains(command, "HOSTNAME=wrapper-host"));
    }
    CHECK(contains(world.container_commands[1], "COMP=gcc") && contains(world.container_commands[2], "COMP=clang"));
    // the copies exist where the container would have the mounts, the writable ones are writable for the build user
    CHECK(fs::exists(world.container_dir.path() / "src" / "src" / "worker" / "worker.cpp"));
    CHECK(fs::exists(world.container_dir.path() / "build.sh"));
    CHECK(fs::exists(world.container_dir.path() / "deps" / "fpm" / "include"));
#ifndef _WIN32
    fs::perms perms = fs::status(world.container_dir.path() / "out").permissions();
    CHECK((perms & fs::perms::others_write) != fs::perms::none);
    CHECK((fs::status(world.container_dir.path() / "src").permissions() & fs::perms::others_write) == fs::perms::none);  // read-only mounts stay as they are
#endif
    // wasm is not built in a container (no Docker): it is carried
    CHECK(world.wasm_builds.empty() && world.docker_calls.empty());
}

TEST(the_in_container_executor_refuses_a_path_without_the_marker_of_a_previous_run_and_replaces_its_own) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    make_container(world);
    // somebody else's /src: nothing is copied, nothing is run, nothing is changed
    write_bytes(world.container_dir.path() / "src" / "precious.txt", "mine");
    std::map<std::string, std::string> before = synthetic.snapshot();
    std::string message = error_of([&] { run_prebuild(container_options(synthetic)); });
    CHECK(message.find("refusing to replace /src in this container: it exists and was not created by a previous in-container run of `prebuild`") == 0);
    CHECK(message.find("nothing was changed") != std::string::npos);
    CHECK_EQ(read_file(world.container_dir.path() / "src" / "precious.txt"), std::string("mine"));
    CHECK(!fs::exists(world.container_dir.path() / "out") && !fs::exists(world.container_dir.path() / ".dc_determinism-prebuild-copies"));
    CHECK(world.container_commands.empty() && synthetic.snapshot() == before);
    // the same for another mount of the list (the writable /out)
    fs::remove_all(world.container_dir.path() / "src");
    write_bytes(world.container_dir.path() / "out" / "precious.txt", "mine");
    CHECK(error_of([&] { run_prebuild(container_options(synthetic)); }).find("refusing to replace /out in this container") == 0);
    CHECK(!fs::exists(world.container_dir.path() / "src") && world.container_commands.empty());
    fs::remove_all(world.container_dir.path() / "out");
    // the run leaves its markers: the next run (the same container, another build) replaces its own copies
    CHECK_EQ(run_prebuild(container_options(synthetic)), 0);
    CHECK(fs::exists(world.container_dir.path() / ".dc_determinism-prebuild-copies" / "src"));
    write_bytes(world.container_dir.path() / "src" / "leftover.txt", "from the previous script");
    CHECK_EQ(run_prebuild(container_options(synthetic)), 0);
    CHECK(!fs::exists(world.container_dir.path() / "src" / "leftover.txt"));
}

TEST(the_in_container_executor_verifies_the_read_only_copies_and_reports_a_failing_script) {
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_container(world);
        world.tamper_src = true;  // the script changes its read-only /src
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_prebuild(container_options(synthetic)); });
        CHECK(message.find("docker build of the linux dc_worker: the read-only copy of /src was changed by the build script") == 0);
        CHECK(synthetic.snapshot() == before);
    }
    {
        SyntheticRoot synthetic;
        FakeWorld world(synthetic.identity.worker_sha256);
        make_container(world);
        world.fail_run = false;
        world.objdump_missing = true;  // the arm64 loader script fails: the same message as the Docker executor
        std::string message = error_of([&] { run_prebuild(container_options(synthetic)); });
        CHECK(message.find("docker linux gcc loader failed (exit code 1):\nobjdump was not found in the image (binutils)") == 0);
    }
}

TEST(an_explicit_emsdk_image_is_refused_inside_a_container) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    make_container(world);
    PrebuildOptions options = container_options(synthetic);
    options.emsdk_image_explicit = true;
    CHECK(error_of([&] { run_prebuild(options); }).find("--emsdk-image needs the Docker CLI and cannot be used inside a container") == 0);
    CHECK(world.docker_calls.empty() && world.container_commands.empty());
}

TEST(a_host_that_owns_wasm_needs_node_before_anything_is_built) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.has_node = false;
    std::map<std::string, std::string> before = synthetic.snapshot();
    std::string message = error_of([&] { run_windows_prebuild(linux_options(synthetic)); });
    CHECK(message.find("node is required on PATH for the host smoke test of the wasm builds") == 0);
    CHECK(message.find("Windows") == std::string::npos);
    CHECK_EQ(build_runs(world), static_cast<std::size_t>(0));
    CHECK(world.builds.empty() && synthetic.snapshot() == before);
    // worker-only builds no wasm: no node needed
    PrebuildOptions options = linux_options(synthetic);
    options.worker_only = true;
    CHECK(error_of([&] { run_windows_prebuild(options); }).empty());
}

// --- `prebuild --check`, the fixed scratch path and the code sections ---------------------------------------------------

namespace {

struct CheckRun {
    int code = -1;
    std::vector<std::string> lines;
};

// The check of the Windows host (every mode of it), with the report lines collected.
CheckRun run_check(PrebuildOptions options) {
    CheckRun result;
    options.check = true;
    prebuild_hooks().report = [&result](const std::string& line) { result.lines.push_back(line); };
    try {
        result.code = run_windows_prebuild(options);
    } catch (...) {
        prebuild_hooks().report = nullptr;
        throw;
    }
    prebuild_hooks().report = nullptr;
    return result;
}

CheckRun run_linux_check(PrebuildOptions options) {
    CheckRun result;
    options.check = true;
    prebuild_hooks().report = [&result](const std::string& line) { result.lines.push_back(line); };
    try {
        result.code = run_linux_prebuild(options);
    } catch (...) {
        prebuild_hooks().report = nullptr;
        throw;
    }
    prebuild_hooks().report = nullptr;
    return result;
}

bool has_line_starting(const std::vector<std::string>& lines, const std::string& prefix) {
    return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.compare(0, prefix.size(), prefix) == 0; });
}

std::size_t docker_runs(const FakeWorld& world) {
    std::size_t runs = 0;
    for (const auto& call : world.docker_calls) runs += (call[0] == "run" && !contains(call, "chown")) ? 1 : 0;
    return runs;
}

PrebuildOptions windows_options(const SyntheticRoot& synthetic) {
    PrebuildOptions options;
    options.root = synthetic.root;
    return options;
}

void edit_manifest(SyntheticRoot& synthetic, const std::string& relative, const std::function<void(Json&)>& change) {
    fs::path path = synthetic.root / path_from_utf8(relative);
    Json value = read_json(path);
    change(value);
    write_bytes(path, manifest_text(value));
}

}  // namespace

TEST(the_toolchain_is_recorded_in_the_manifests) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    Json manifest = synthetic.read();
    CHECK((keys_of(manifest["toolchain"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "wasm"}));
    CHECK((keys_of(manifest["toolchain"]["windows-amd64"]) == std::vector<std::string>{"msvc", "gcc", "clang"}));
    CHECK_EQ(manifest["toolchain"]["windows-amd64"]["msvc"]["toolset_dir"], Json("14.52.37025"));
    CHECK_EQ(manifest["toolchain"]["windows-amd64"]["gcc"]["tag"], Json("1"));
    CHECK_EQ(manifest["toolchain"]["linux-amd64"], Json::parse(R"({"image": "dc_determinism-cpu:local", "image_id": "sha256:id-of-dc_determinism-cpu:local"})"));
    CHECK_EQ(manifest["toolchain"]["wasm"]["image_id"], Json("sha256:id-of-emscripten/emsdk:6.0.9"));
    // The Windows loader manifest records the same facts; the Linux one has had its image id all along.
    Json loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    CHECK_EQ(loaders["toolchain"], manifest["toolchain"]["windows-amd64"]);
    CHECK((keys_of(loaders) == std::vector<std::string>{"schema", "image_id", "source", "dependencies", "toolchain", "libraries"}));
}

TEST(a_worker_only_rebuild_records_only_the_toolchain_it_used) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    world.toolchain_tag = "2";
    PrebuildOptions options = windows_options(synthetic);
    options.worker_only = true;
    run_windows_prebuild(options);
    Json manifest = synthetic.read();
    CHECK_EQ(manifest["toolchain"]["windows-amd64"]["gcc"]["tag"], Json("2"));  // the compiler that was used
    CHECK_EQ(manifest["toolchain"]["windows-amd64"]["msvc"]["tag"], Json("1"));  // the others were not
    CHECK_EQ(manifest["toolchain"]["windows-amd64"]["clang"]["tag"], Json("1"));
    CHECK(manifest["toolchain"].contains("wasm"));
}

TEST(the_check_reports_every_artifact_and_manifest_identical_and_installs_nothing) {
    SyntheticRoot synthetic;
    synthetic.add_arm64();
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    std::map<std::string, std::string> installed = synthetic.snapshot();
    std::size_t runs = docker_runs(world);

    CheckRun result = run_check(windows_options(synthetic));
    CHECK_EQ(result.code, 0);
    CHECK(synthetic.snapshot() == installed);  // nothing was installed
    CHECK(docker_runs(world) > runs);          // everything was rebuilt
    // Two dc_worker, five loaders, the wasm bundle, twelve players and four manifests; the linux-arm64 data is not looked at.
    CHECK_EQ(result.lines.size(), static_cast<std::size_t>(2 + 5 + 1 + 12 + 4));
    for (const std::string& line : result.lines) CHECK(ends_with(line, ": identical"));
    for (const char* wanted :
         {"dc_worker windows-amd64: identical", "dc_worker linux-amd64: identical", "loader windows-amd64/msvc: identical",
          "loader windows-amd64/gcc: identical", "loader windows-amd64/clang: identical", "loader linux-amd64/gcc: identical",
          "loader linux-amd64/clang: identical", "wasm dc_worker: identical", "player windows-amd64-msvc/normal_dist: identical",
          "player linux-amd64-gcc/nd2: identical", "player wasm/nd2: identical", "manifest prebuilt/manifest.json: identical",
          "manifest prebuilt/loaders/windows-amd64/manifest.json: identical",
          "manifest prebuilt/loaders/linux-amd64/manifest.json: identical", "manifest prebuilt/worker/wasm/manifest.json: identical"}) {
        CHECK(contains(result.lines, wanted));
    }
    // The scratch directory and its lock are gone.
    CHECK(!fs::exists(world.scratch_dir.path() / "pb"));
    CHECK(!fs::exists(world.scratch_dir.path() / "pb.lock"));
}

TEST(the_check_exits_1_with_a_line_per_differing_artifact_when_the_toolchain_is_the_same) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    std::map<std::string, std::string> installed = synthetic.snapshot();

    world.build_salt = "-changed";  // every build now gives other bytes
    CheckRun result = run_check(windows_options(synthetic));
    CHECK_EQ(result.code, 1);
    CHECK(synthetic.snapshot() == installed);
    CHECK(has_line_starting(result.lines, "dc_worker windows-amd64: differs (dc_worker.exe installed "));
    CHECK(has_line_starting(result.lines, "dc_worker linux-amd64: differs ("));
    CHECK(has_line_starting(result.lines, "loader windows-amd64/msvc: differs ("));
    CHECK(has_line_starting(result.lines, "loader linux-amd64/clang: differs ("));
    CHECK(has_line_starting(result.lines, "player linux-amd64-gcc/nd2: differs ("));
    CHECK(has_line_starting(result.lines, "player windows-amd64-clang/normal_dist: differs ("));
    CHECK(contains(result.lines, "wasm dc_worker: identical"));    // the wasm bundle did not change
    CHECK(contains(result.lines, "player wasm/nd2: identical"));
    // The manifests name the members that differ: the digests of what was rebuilt.
    CHECK(has_line_starting(result.lines, "manifest prebuilt/manifest.json: differs (binaries.linux-amd64.sha256"));
    CHECK(has_line_starting(result.lines, "manifest prebuilt/loaders/linux-amd64/manifest.json: differs (libraries."));
    // The fake binaries are neither PE nor ELF: no line about code sections.
    CHECK(!has_line_starting(result.lines, "dc_worker windows-amd64: code sections"));
}

TEST(a_differing_pe_or_elf_file_gets_a_second_line_about_its_code_sections) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.elf_worker = true;
    run_windows_prebuild(windows_options(synthetic));

    world.build_salt = "-metadata";  // only the note around the code changes
    CheckRun metadata = run_check(windows_options(synthetic));
    CHECK_EQ(metadata.code, 1);
    CHECK(has_line_starting(metadata.lines, "dc_worker linux-amd64: differs ("));
    CHECK(contains(metadata.lines, "dc_worker linux-amd64: code sections identical (.text)"));

    world.worker_text = "other code";  // the generated code changes
    CheckRun code = run_check(windows_options(synthetic));
    CHECK_EQ(code.code, 1);
    CHECK(contains(code.lines, "dc_worker linux-amd64: code sections differ (.text)"));
    // The second line follows the first.
    for (std::size_t i = 0; i + 1 < code.lines.size(); ++i) {
        if (has_line_starting(std::vector<std::string>{code.lines[i]}, "dc_worker linux-amd64: differs")) {
            CHECK(has_line_starting(std::vector<std::string>{code.lines[i + 1]}, "dc_worker linux-amd64: code sections"));
        }
    }
}

TEST(the_check_compares_the_toolchain_first_and_builds_nothing_when_it_differs) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    std::size_t runs = docker_runs(world);
    std::size_t cmake = world.cmake_calls.size();
    std::size_t builds = world.builds.size();

    world.toolchain_tag = "2";  // another compiler, linker, SDK ...
    CheckRun compilers = run_check(windows_options(synthetic));
    CHECK_EQ(compilers.code, 2);
    CHECK(has_line_starting(compilers.lines, "toolchain differs: prebuilt/manifest.json toolchain.windows-amd64.clang.tag: recorded '1', current '2'; "));
    CHECK(std::any_of(compilers.lines.begin(), compilers.lines.end(), [](const std::string& line) {
        return line.find("prebuilt/manifest.json toolchain.windows-amd64.msvc.tag: recorded '1', current '2'") != std::string::npos;
    }));
    CHECK(compilers.lines.back().find("nothing was rebuilt") != std::string::npos);
    CHECK(has_line_starting(compilers.lines, "toolchain differs: prebuilt/loaders/windows-amd64/manifest.json toolchain.msvc.tag"));
    for (const std::string& line : compilers.lines) CHECK(line.find(": identical") == std::string::npos && line.find(": differs (") == std::string::npos);
    CHECK_EQ(docker_runs(world), runs);  // nothing was built
    CHECK_EQ(world.cmake_calls.size(), cmake);
    CHECK_EQ(world.builds.size(), builds);

    world.toolchain_tag = "1";
    world.image_id_tag = "-rebuilt";  // another Docker image
    CheckRun images = run_check(windows_options(synthetic));
    CHECK_EQ(images.code, 2);
    CHECK(has_line_starting(images.lines, "toolchain differs: prebuilt/manifest.json toolchain.linux-amd64.image_id: recorded 'sha256:id-of-dc_determinism-cpu:local', current 'sha256:id-of-dc_determinism-cpu:local-rebuilt'"));
    CHECK(has_line_starting(images.lines, "toolchain differs: prebuilt/loaders/linux-amd64/manifest.json image_id: "));
    // the other id of the Emscripten image is not the recorded one: this host does not own wasm, so it is not compared
    for (const std::string& line : images.lines) CHECK(line.find("wasm") == std::string::npos);
    CHECK_EQ(docker_runs(world), runs);

    world.image_id_tag = "";
    CHECK_EQ(run_check(windows_options(synthetic)).code, 0);  // the recorded toolchain again: the rebuild runs
}

TEST(a_manifest_without_the_toolchain_facts_gives_toolchain_not_recorded) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    // The manifests as the sources of the previous version wrote them: no toolchain facts anywhere.
    CheckRun fresh = run_check(windows_options(synthetic));
    CHECK_EQ(fresh.code, 2);
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.windows-amd64"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.linux-amd64"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/loaders/windows-amd64/manifest.json toolchain.msvc"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/loaders/linux-amd64/manifest.json has no image_id"));
    CHECK_EQ(docker_runs(world), static_cast<std::size_t>(0));
    CHECK(world.cmake_calls.empty() && world.builds.empty());

    run_windows_prebuild(windows_options(synthetic));
    CHECK_EQ(run_check(windows_options(synthetic)).code, 0);
    // Each manifest on its own.
    edit_manifest(synthetic, "prebuilt/manifest.json", [](Json& value) { value.erase("toolchain"); });
    CheckRun main_manifest = run_check(windows_options(synthetic));
    CHECK_EQ(main_manifest.code, 2);
    CHECK(has_line_starting(main_manifest.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.windows-amd64"));
    run_windows_prebuild(windows_options(synthetic));
    edit_manifest(synthetic, "prebuilt/loaders/windows-amd64/manifest.json", [](Json& value) { value.erase("toolchain"); });
    CheckRun loader_manifest = run_check(windows_options(synthetic));
    CHECK_EQ(loader_manifest.code, 2);
    CHECK(has_line_starting(loader_manifest.lines, "toolchain not recorded: prebuilt/loaders/windows-amd64/manifest.json toolchain.msvc"));
    run_windows_prebuild(windows_options(synthetic));
    edit_manifest(synthetic, "prebuilt/worker/wasm/manifest.json", [](Json& value) { value["toolchain"].erase("image_id"); });
    CheckRun wasm_manifest = run_check(windows_options(synthetic));
    CHECK_EQ(wasm_manifest.code, 2);
    CHECK(has_line_starting(wasm_manifest.lines, "toolchain not recorded: prebuilt/worker/wasm/manifest.json has no toolchain.image_id"));
}

TEST(a_member_that_differs_in_a_manifest_is_reported_by_name_without_a_byte_comparison) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    // Re-writing the manifest with other key order and spacing does not matter: the members are compared.
    Json manifest = synthetic.read();
    Json reordered = Json::object();
    for (auto it = manifest.end(); it != manifest.begin();) {
        --it;
        reordered[it.key()] = it.value();
    }
    write_bytes(synthetic.root / "prebuilt" / "manifest.json", json_dumps(reordered, DumpOptions::pretty(4)) + "\n");
    CHECK_EQ(run_check(windows_options(synthetic)).code, 0);
    // A changed member is named.
    edit_manifest(synthetic, "prebuilt/manifest.json", [](Json& value) { value["binaries"]["windows-amd64"]["compiler"] = "g++ 1.0"; });
    CheckRun result = run_check(windows_options(synthetic));
    CHECK_EQ(result.code, 1);
    CHECK(contains(result.lines, "manifest prebuilt/manifest.json: differs (binaries.windows-amd64.compiler)"));
    // The artifacts did not change.
    CHECK(contains(result.lines, "dc_worker windows-amd64: identical"));
}

TEST(prebuild_never_reuses_the_build_cache) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    testing::TempDir cache;
    build_hooks().cache = cache.path();
    run_windows_prebuild(windows_options(synthetic));
    CHECK_EQ(world.cmake_calls.size(), static_cast<std::size_t>(3 * 2));
    CHECK(fs::exists(cache.path() / "gcc" / "loader" / kBuildReceiptName));  // the first run left a valid cache behind
    // The second run has the same root, sources and flags, so a cache would match; it is ignored and the loaders are
    // built again (with another result, which the check sees).
    world.build_salt = "-second";
    CheckRun second = run_check(windows_options(synthetic));
    CHECK_EQ(world.cmake_calls.size(), static_cast<std::size_t>(2 * 3 * 2));
    CHECK_EQ(second.code, 1);
    CHECK(has_line_starting(second.lines, "loader windows-amd64/gcc: differs ("));
    build_hooks().cache = fs::path();
}

TEST(the_builds_run_at_the_fixed_scratch_path_and_never_below_the_checkout) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    std::string scratch = SyntheticRoot::to_posix_text(world.scratch_dir.path() / "pb");
    std::string checkout = SyntheticRoot::to_posix_text(synthetic.root);
    auto mentions = [](const std::vector<std::string>& argv, const std::string& text) {
        return std::any_of(argv.begin(), argv.end(), [&](const std::string& item) {
            std::string posix = item;
            std::replace(posix.begin(), posix.end(), '\\', '/');
            return posix.find(text) != std::string::npos;
        });
    };
    CHECK(!world.builds.empty() && !world.cmake_calls.empty());
    for (const auto& argv : world.builds) {
        CHECK(mentions(argv, scratch));
        CHECK(!mentions(argv, checkout));
    }
    for (const auto& argv : world.cmake_calls) {
        CHECK(mentions(argv, scratch));
        CHECK(!mentions(argv, checkout));
    }
    // The containers get the copy, not the checkout (the vendored trees, the worker sources).
    for (const auto& call : world.docker_calls) {
        if (call[0] != "run") continue;
        for (const auto& mount : mounts_of(call)) {
            if (mount.first == "/src:ro" || mount.first == "/repo:ro") CHECK(mentions({mount.second}, scratch));
        }
    }
    CHECK(!fs::exists(synthetic.root / "build"));  // no build directory below the checkout
    CHECK(!fs::exists(world.scratch_dir.path() / "pb"));
}

TEST(a_second_run_at_the_same_time_is_refused_before_anything_is_built) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    std::map<std::string, std::string> before = synthetic.snapshot();
    {
        ScratchDirectory other(world.scratch_dir.path() / "pb");
        std::string message = error_of([&] { run_windows_prebuild(windows_options(synthetic)); });
        CHECK(message.find("another run is using the scratch directory ") == 0);
        CHECK_EQ(docker_runs(world), static_cast<std::size_t>(0));
        CHECK(world.cmake_calls.empty() && world.builds.empty());
    }
    CHECK(synthetic.snapshot() == before);
    CHECK(error_of([&] { run_windows_prebuild(windows_options(synthetic)); }).empty());  // free again
}

TEST(the_scratch_directory_is_exclusive_emptied_and_removed) {
    testing::TempDir temp;
    fs::path path = temp.path() / "parent" / "b";
    write_bytes(path / ".dc_determinism-scratch", "");
    write_bytes(path / "stale.txt", "left behind by a crashed run");
    {
        ScratchDirectory first(path);
        CHECK(fs::is_directory(path) && !fs::exists(path / "stale.txt"));
        write_bytes(path / "work.txt", "x");
        CHECK(error_of([&] { ScratchDirectory second(path); }).find("another run is using the scratch directory ") == 0);
        CHECK(fs::exists(path / "work.txt"));  // the refused run did not touch it
    }
    CHECK(!fs::exists(path));
    CHECK(!fs::exists(temp.path() / "parent" / "b.lock"));
    { ScratchDirectory again(path); }
    CHECK(error_of([] { ScratchDirectory relative(fs::path("relative")); }).find("must be absolute") != std::string::npos);
    CHECK(!default_scratch_path().empty() && default_scratch_path().is_absolute());
}

TEST(the_reproducibility_flags_are_the_ones_that_remove_timestamps_and_paths) {
    fs::path root = fs::path(is_windows() ? "C:\\dc_determinism-pb\\b" : "/tmp/dc_determinism-pb/b");
    ReproducibleFlags msvc = reproducible_flags("msvc", root);
    ReproducibleFlags gcc = reproducible_flags("gcc", root);
    ReproducibleFlags clang = reproducible_flags("clang", root);
    ReproducibleFlags wasm = reproducible_flags("wasm", root);
    if (!is_windows()) {  // the Linux and wasm builds run in Docker at fixed paths
        CHECK(msvc.compile.empty() && msvc.link.empty() && gcc.compile.empty() && gcc.link.empty());
        return;
    }
    CHECK((msvc.compile == std::vector<std::string>{"/Brepro"} && msvc.link == std::vector<std::string>{"/Brepro"}));
    CHECK((gcc.compile == std::vector<std::string>{"-ffile-prefix-map=C:/dc_determinism-pb/b=."}));
    CHECK((gcc.link == std::vector<std::string>{"-Wl,--no-insert-timestamp"}));
    CHECK((clang.compile == gcc.compile && clang.link == gcc.link));
    CHECK(wasm.compile.empty() && wasm.link.empty());
}

TEST(the_loader_recipe_passes_the_reproducibility_flags_to_cmake_on_windows) {
    if (!is_windows()) return;
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    auto configure = [&](const std::string& compiler) {
        for (const auto& argv : world.cmake_calls) {
            if (argv.size() > 1 && argv[1] == "-S" && contains(argv, std::string("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")) &&
                std::any_of(argv.begin(), argv.end(), [&](const std::string& item) { return ends_with(item, "/" + compiler + "/loader/cmake"); })) {
                return argv;
            }
        }
        return std::vector<std::string>();
    };
    std::vector<std::string> msvc = configure("msvc");
    CHECK(contains(msvc, "-DCMAKE_CXX_FLAGS_INIT=/Brepro") && contains(msvc, "-DCMAKE_C_FLAGS_INIT=/Brepro"));
    CHECK(contains(msvc, "-DCMAKE_SHARED_LINKER_FLAGS_INIT=/Brepro") && contains(msvc, "-DCMAKE_MODULE_LINKER_FLAGS_INIT=/Brepro"));
    std::vector<std::string> gcc = configure("gcc");
    CHECK(contains(gcc, "-DCMAKE_SHARED_LINKER_FLAGS_INIT=-Wl,--no-insert-timestamp"));
    CHECK(std::any_of(gcc.begin(), gcc.end(), [](const std::string& item) {
        return item.compare(0, 36, "-DCMAKE_CXX_FLAGS_INIT=-ffile-prefix") == 0;
    }));
    // The recorded recipe names the fixed path as <repo>, so it does not depend on the scratch path chosen.
    Json loaders = read_json(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    bool recorded = false;
    for (const Json& flag : loaders["libraries"]["gcc"]["build"]["configure"]) {
        recorded = recorded || flag == Json("-DCMAKE_CXX_FLAGS_INIT=-ffile-prefix-map=<repo>=.");
    }
    CHECK(recorded);
}

TEST(the_worker_only_check_looks_at_the_two_dc_worker_binaries_only) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions options = windows_options(synthetic);
    options.worker_only = true;
    run_windows_prebuild(options);
    CheckRun result = run_check(options);
    CHECK_EQ(result.code, 0);
    CHECK((result.lines == std::vector<std::string>{"dc_worker windows-amd64: identical", "dc_worker linux-amd64: identical",
                                                    "manifest prebuilt/manifest.json: identical"}));
    world.build_salt = "-x";
    CheckRun changed = run_check(options);
    CHECK_EQ(changed.code, 1);
    CHECK(has_line_starting(changed.lines, "dc_worker linux-amd64: differs ("));
}

TEST(the_worker_only_check_after_a_full_prebuild_compares_only_the_compiler_it_uses) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);  // the manifests record msvc, gcc and clang
    PrebuildOptions options = windows_options(synthetic);
    options.worker_only = true;
    CheckRun same = run_check(options);
    CHECK_EQ(same.code, 0);
    CHECK((same.lines == std::vector<std::string>{"dc_worker windows-amd64: identical", "dc_worker linux-amd64: identical",
                                                  "manifest prebuilt/manifest.json: identical"}));
    world.toolchain_tag = "2";  // the gcc the mode uses changed
    CheckRun changed = run_check(options);
    CHECK_EQ(changed.code, 2);
    CHECK(has_line_starting(changed.lines, "toolchain differs: prebuilt/manifest.json toolchain.windows-amd64.gcc.tag: recorded '1', current '2'"));
    CHECK(!std::any_of(changed.lines.begin(), changed.lines.end(), [](const std::string& line) {
        return line.find("toolchain.windows-amd64.msvc") != std::string::npos || line.find("toolchain.windows-amd64.clang") != std::string::npos;
    }));
}

TEST(the_scratch_directory_never_deletes_what_it_did_not_create) {
    testing::TempDir temp;
    fs::path path = temp.path() / "parent" / "b";
    write_bytes(path / "precious.txt", "somebody else's file");
    std::string message = error_of([&] { ScratchDirectory scratch(path); });
    CHECK(message.find("refusing to use ") == 0);
    CHECK(message.find("nothing was deleted") != std::string::npos);
    CHECK(fs::exists(path / "precious.txt"));
    CHECK(!fs::exists(temp.path() / "parent" / "b.lock"));  // the lock was released
    // An empty directory is taken (and removed afterwards); one made by the tool (marker) is taken again after a crash.
    fs::remove(path / "precious.txt");
    { ScratchDirectory empty(path); CHECK(fs::exists(path / ".dc_determinism-scratch")); }
    CHECK(!fs::exists(path));
    write_bytes(path / ".dc_determinism-scratch", "");
    write_bytes(path / "stale.txt", "x");
    { ScratchDirectory again(path); CHECK(!fs::exists(path / "stale.txt")); }
    // A file system root is refused.
    fs::path root = temp.path().root_path();
    CHECK(error_of([&] { ScratchDirectory scratch(root); }).find("must not be") != std::string::npos);
    // The prebuild itself refuses before building anything (DC_DETERMINISM_PREBUILD_SCRATCH may name any directory).
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    write_bytes(world.scratch_dir.path() / "pb" / "keep.txt", "keep");
    std::string refused = error_of([&] { run_windows_prebuild(windows_options(synthetic)); });
    CHECK(refused.find("refusing to use ") == 0);
    CHECK(fs::exists(world.scratch_dir.path() / "pb" / "keep.txt"));
    CHECK(world.cmake_calls.empty() && world.builds.empty());
}

TEST(the_wasm_worker_check_has_exit_codes_and_does_not_read_the_installed_loader_manifests) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions options = wasm_options(synthetic);
    CHECK_EQ(run_prebuild(options), 0);
    options.check = true;
    CheckRun result;
    prebuild_hooks().report = [&result](const std::string& line) { result.lines.push_back(line); };
    // The installed loader manifests are not consulted: a check without any of them still runs.
    fs::remove(synthetic.root / "prebuilt" / "loaders" / "windows-amd64" / "manifest.json");
    fs::remove(synthetic.root / "prebuilt" / "loaders" / "linux-amd64" / "manifest.json");
    result.code = run_prebuild(options);
    CHECK_EQ(result.code, 0);
    CHECK((result.lines == std::vector<std::string>{"wasm dc_worker: identical", "manifest prebuilt/worker/wasm/manifest.json: identical"}));
    result.lines.clear();
    world.wasm_content = "v2";
    CHECK_EQ(run_prebuild(options), 1);
    CHECK(has_line_starting(result.lines, "wasm dc_worker: differs (dc_worker.js installed "));
    result.lines.clear();
    world.image_id_tag = "-new";
    CHECK_EQ(run_prebuild(options), 2);
    CHECK(has_line_starting(result.lines, "toolchain differs: prebuilt/worker/wasm/manifest.json toolchain.image_id: "));
    prebuild_hooks().report = nullptr;
}

TEST(the_check_of_an_x86_64_linux_host_covers_its_own_platform_only) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    PrebuildOptions options = windows_options(synthetic);
    CheckRun fresh = run_linux_check(options);
    CHECK_EQ(fresh.code, 2);  // no toolchain recorded yet: the platforms of this host first
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.linux-amd64"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.wasm"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/loaders/linux-amd64/manifest.json has no image_id"));
    CHECK(!has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.windows-amd64"));
    CHECK_EQ(build_runs(world), static_cast<std::size_t>(0));
    CHECK_EQ(run_linux_prebuild(options), 0);
    Json installed_manifest = synthetic.read();
    CHECK_EQ(installed_manifest["toolchain"]["linux-amd64"]["image_id"], Json("sha256:id-of-dc_determinism-cpu:local"));

    CheckRun same = run_linux_check(options);
    CHECK_EQ(same.code, 0);
    // the worker, two loaders and their manifest, the wasm bundle and its manifest, six players, the main manifest
    CHECK_EQ(same.lines.size(), static_cast<std::size_t>(1 + 2 + 1 + 2 + 6 + 1));
    for (const std::string& line : same.lines) {
        CHECK(ends_with(line, ": identical"));
        CHECK(line.find("windows-amd64") == std::string::npos && line.find("linux-arm64") == std::string::npos);
    }
    for (const char* wanted : {"dc_worker linux-amd64: identical", "loader linux-amd64/gcc: identical", "loader linux-amd64/clang: identical",
                               "manifest prebuilt/loaders/linux-amd64/manifest.json: identical", "wasm dc_worker: identical",
                               "player linux-amd64-gcc/nd2: identical", "player wasm/nd2: identical", "manifest prebuilt/manifest.json: identical"}) {
        CHECK(contains(same.lines, wanted));
    }
    world.build_salt = "-y";
    CheckRun changed = run_linux_check(options);
    CHECK_EQ(changed.code, 1);
    CHECK(has_line_starting(changed.lines, "dc_worker linux-amd64: differs ("));
    CHECK(has_line_starting(changed.lines, "loader linux-amd64/gcc: differs ("));
    CHECK(has_line_starting(changed.lines, "manifest prebuilt/loaders/linux-amd64/manifest.json: differs (libraries."));
    world.build_salt = "";
    world.image_id_tag = "-z";
    CheckRun other_image = run_linux_check(options);
    CHECK_EQ(other_image.code, 2);
    CHECK(has_line_starting(other_image.lines, "toolchain differs: prebuilt/manifest.json toolchain.linux-amd64.image_id: "));
    CHECK(has_line_starting(other_image.lines, "toolchain differs: prebuilt/loaders/linux-amd64/manifest.json image_id: "));
    world.image_id_tag = "";
    // Sources that no longer match the manifest or the loader manifests: nothing to compare the installed artifacts with.
    write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int other;\n");
    CheckRun stale = run_linux_check(options);
    CHECK_EQ(stale.code, 1);
    CHECK(has_line_starting(stale.lines, "manifest prebuilt/manifest.json: differs (worker.cpp changed"));
    write_bytes(synthetic.root / "src" / "worker" / "worker.cpp", "int worker_cpp;\n");
    edit_manifest(synthetic, "prebuilt/loaders/linux-amd64/manifest.json", [](Json& value) { value["source"]["revision"] = "other"; });
    CheckRun loaders = run_linux_check(options);
    CHECK_EQ(loaders.code, 1);
    CHECK(has_line_starting(loaders.lines, "manifest prebuilt/manifest.json: differs (the loader source differs from prebuilt/loaders/linux-amd64/manifest.json"));
}

TEST(the_check_of_an_aarch64_host_covers_linux_arm64_and_its_toolchain) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    make_arm64_host(world);
    PrebuildOptions options = windows_options(synthetic);
    CheckRun fresh = run_linux_check(options);
    CHECK_EQ(fresh.code, 2);
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/manifest.json toolchain.linux-arm64"));
    CHECK(has_line_starting(fresh.lines, "toolchain not recorded: prebuilt/loaders/linux-arm64/manifest.json has no image_id"));
    CHECK_EQ(run_linux_prebuild(options), 0);

    CheckRun same = run_linux_check(options);
    CHECK_EQ(same.code, 0);
    // the worker, four loaders and their manifest, eight players, the main manifest; no wasm (carried), nothing amd64
    CHECK_EQ(same.lines.size(), static_cast<std::size_t>(1 + 4 + 1 + 8 + 1));
    for (const std::string& line : same.lines) {
        CHECK(ends_with(line, ": identical"));
        CHECK(line.find("windows-amd64") == std::string::npos && line.find("linux-amd64") == std::string::npos && line.find("wasm") == std::string::npos);
    }
    for (const char* wanted : {"dc_worker linux-arm64: identical", "loader linux-arm64/gcc-nofma: identical", "loader linux-arm64/clang-nofma: identical",
                               "player linux-arm64-clang-nofma/nd2: identical", "manifest prebuilt/loaders/linux-arm64/manifest.json: identical",
                               "manifest prebuilt/manifest.json: identical"}) {
        CHECK(contains(same.lines, wanted));
    }
    // the fused multiply-add counts are part of the loader manifest
    world.fma_counts["gcc"] = 99;
    CheckRun counts = run_linux_check(options);
    CHECK_EQ(counts.code, 1);
    CHECK(has_line_starting(counts.lines, "manifest prebuilt/loaders/linux-arm64/manifest.json: differs (libraries.gcc.build.fused_multiply_add_instructions"));
    world.fma_counts.clear();
    world.fma_counts["clang-nofma"] = 1;  // the gate also stops a check
    CHECK(error_of([&] { run_linux_check(options); }).find("the clang-nofma loader contains 1 fused multiply-add instructions") == 0);
    world.fma_counts.clear();
    world.build_salt = "-y";
    CheckRun changed = run_linux_check(options);
    CHECK_EQ(changed.code, 1);
    CHECK(has_line_starting(changed.lines, "loader linux-arm64/gcc: differs ("));
    CHECK(has_line_starting(changed.lines, "player linux-arm64-gcc-nofma/normal_dist: differs ("));
    world.build_salt = "";
    world.image_id_tag = "-z";
    CheckRun other_image = run_linux_check(options);
    CHECK_EQ(other_image.code, 2);
    CHECK(has_line_starting(other_image.lines, "toolchain differs: prebuilt/manifest.json toolchain.linux-arm64.image_id: "));
    CHECK(has_line_starting(other_image.lines, "toolchain differs: prebuilt/loaders/linux-arm64/manifest.json image_id: "));
}

TEST(the_windows_check_does_not_look_at_the_carried_linux_arm64_data) {
    SyntheticRoot synthetic;
    synthetic.add_arm64();
    FakeWorld world(synthetic.identity.worker_sha256);
    run_windows_prebuild(windows_options(synthetic));
    std::map<std::string, std::string> installed = synthetic.snapshot();
    // damage the carried data: it is neither compared nor changed
    write_bytes(synthetic.root / "prebuilt" / "worker" / "linux-arm64" / "dc_worker", "damaged");
    edit_manifest(synthetic, "prebuilt/manifest.json", [](Json& value) { value["toolchain"]["linux-arm64"]["image_id"] = "sha256:other"; });
    CheckRun result = run_check(windows_options(synthetic));
    CHECK_EQ(result.code, 0);
    for (const std::string& line : result.lines) CHECK(line.find("linux-arm64") == std::string::npos);
    std::map<std::string, std::string> damaged = synthetic.snapshot();
    CHECK_EQ(damaged.at("worker/linux-arm64/dc_worker"), sha256_hex("damaged"));
    CHECK(installed.at("manifest.json") != synthetic.snapshot().at("manifest.json"));  // only my own edit
}

TEST(a_host_that_does_not_own_wasm_carries_it_and_leaves_toolchain_wasm_out_of_the_check) {
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);  // nothing recorded: this host owns wasm
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(1));
    Json recorded = synthetic.read();
    CHECK_EQ(recorded["toolchain"]["wasm"]["image_id"], Json("sha256:id-of-emscripten/emsdk:6.0.9"));
    // the recorded image again: owned (a rebuild)
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(2));

    // another Emscripten image id: not owned, so the check leaves wasm out (toolchain, artifacts and manifests)
    world.emsdk_id_tag = "-other";
    CheckRun check = run_check(windows_options(synthetic));
    CHECK_EQ(check.code, 0);
    CHECK_EQ(check.lines.size(), static_cast<std::size_t>(2 + 5 + 10 + 3));
    for (const std::string& line : check.lines) CHECK(line.find("wasm") == std::string::npos);
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(2));
    // ... and a rebuild carries the wasm members: the bundle, its manifest, the wasm players and toolchain.wasm
    std::map<std::string, std::string> before = synthetic.snapshot();
    const std::size_t earlier_calls = world.docker_calls.size();
    world.build_salt = "-second";
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);
    Json manifest = synthetic.read();
    CHECK_EQ(manifest["toolchain"]["wasm"], recorded["toolchain"]["wasm"]);
    CHECK_EQ(manifest["players"]["binaries"]["wasm/nd2"], recorded["players"]["binaries"]["wasm/nd2"]);
    CHECK_EQ(manifest["players"]["binaries"]["wasm/normal_dist"], recorded["players"]["binaries"]["wasm/normal_dist"]);
    for (const std::string& file : changed_files(before, synthetic.snapshot())) {
        CHECK(file.compare(0, 11, "worker/wasm") != 0 && file.compare(0, 13, "players/wasm/") != 0);
    }
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(2));  // no Emscripten container ran
    for (std::size_t i = earlier_calls; i < world.docker_calls.size(); ++i) {
        if (world.docker_calls[i][0] == "run") CHECK(!contains(world.docker_calls[i], "emscripten/emsdk:6.0.9"));
    }
}

TEST(a_host_that_does_not_own_wasm_stops_with_exit_2_when_the_identities_wasm_depends_on_changed) {
    struct Case {
        const char* name;
        std::function<void(SyntheticRoot&, FakeWorld&)> change;
        const char* reason;
    };
    const std::vector<Case> cases = {
        {"worker.cpp", [](SyntheticRoot& s, FakeWorld& w) {
             write_bytes(s.root / "src" / "worker" / "worker.cpp", "int changed_worker;\n");
             w.worker_sha = sha256_lf_file(s.root / "src" / "worker" / "worker.cpp");
         }, "worker.cpp changed (sha256 "},
        {"player.cpp", [](SyntheticRoot& s, FakeWorld&) { write_bytes(s.root / "src" / "worker" / "player.cpp", "int changed_player_worker;\n"); },
         "the player sources changed: src/worker/player.cpp changed"},
        {"loaders", [](SyntheticRoot& s, FakeWorld&) {
             edit_manifest(s, "prebuilt/worker/wasm/manifest.json", [](Json& value) { value["simulator"]["source"]["revision"] = "x"; });
         }, "the loader source or dependencies changed"},
    };
    for (const Case& item : cases) {
        for (const char* why : {"another id", "image absent"}) {
            SyntheticRoot synthetic;
            FakeWorld world(synthetic.identity.worker_sha256);
            CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);
            if (std::string(why) == "another id") world.emsdk_id_tag = "-other";
            else world.present.erase("emscripten/emsdk:6.0.9");
            item.change(synthetic, world);
            std::map<std::string, std::string> before = synthetic.snapshot();
            std::size_t runs = build_runs(world);
            std::string message = error_of([&] { run_windows_prebuild(windows_options(synthetic)); });
            CHECK(message.find("the WebAssembly dc_worker and players cannot be carried unchanged: ") == 0);
            CHECK(message.find(item.reason) != std::string::npos);
            // the recorded Emscripten image is named
            CHECK(message.find("Rebuild them with the recorded Emscripten image emscripten/emsdk:6.0.9 (sha256:id-of-emscripten/emsdk:6.0.9)") != std::string::npos);
            CHECK(synthetic.snapshot() == before);  // nothing was installed
            CHECK_EQ(build_runs(world), runs);      // and nothing was built
        }
    }
    // the same sources: carried, not an error (another id, and an image that is not there)
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);
    world.present.erase("emscripten/emsdk:6.0.9");
    CHECK_EQ(run_windows_prebuild(windows_options(synthetic)), 0);
    Json after_carry = synthetic.read();
    CHECK_EQ(after_carry["toolchain"]["wasm"]["image_id"], Json("sha256:id-of-emscripten/emsdk:6.0.9"));
    CHECK_EQ(world.wasm_builds.size(), static_cast<std::size_t>(1));
    // --emsdk-image names an image that is not there: an error, before anything is built
    PrebuildOptions explicit_image = windows_options(synthetic);
    explicit_image.emsdk_image_explicit = true;
    std::size_t runs = build_runs(world);
    CHECK_EQ(error_of([&] { run_windows_prebuild(explicit_image); }), std::string("Docker image emscripten/emsdk:6.0.9 not found (--emsdk-image)"));
    CHECK_EQ(build_runs(world), runs);
    // nothing recorded and no image: there is nothing to carry (wasm is simply not part of this run)
    SyntheticRoot fresh;
    FakeWorld fresh_world(fresh.identity.worker_sha256);
    fresh_world.present.erase("emscripten/emsdk:6.0.9");
    CHECK_EQ(run_windows_prebuild(windows_options(fresh)), 0);
    CHECK(!fs::exists(fresh.root / "prebuilt" / "worker" / "wasm"));
    CHECK(!fresh.read()["toolchain"].contains("wasm"));
}

// --- `prebuild --tool-fragment`: the dc_determinism section ----------------------------------------------------------------

namespace {

// The host the tool mode believes it runs on, for the length of a test.
struct ToolHost {
    explicit ToolHost(bool windows) { prebuild_hooks().windows_host = [windows] { return windows; }; }
    ~ToolHost() { prebuild_hooks().windows_host = nullptr; }
};

std::string tool_binary_name(const std::string& platform) { return platform == "windows-amd64" ? "dc_determinism.exe" : "dc_determinism"; }

// A fragment of one platform: its built binary below build/fragments/<platform>/ and the fragment file next to it.
std::string write_fragment(const SyntheticRoot& synthetic, const std::string& platform, const std::string& content,
                           const std::string& tag = "1", bool with_built = true) {
    static int serial = 0;  // every fragment has a directory of its own: an earlier fragment stays valid
    ++serial;
    const std::string folder = "build/fragments/" + std::to_string(serial) + "/" + platform;
    fs::path directory = synthetic.root / path_from_utf8(folder);
    std::string name = tool_binary_name(platform);
    write_bytes(directory / name, content);
    Json fragment = Json::object();
    fragment["platform"] = platform;
    fragment["path"] = "prebuilt/dc_determinism/" + platform + "/" + name;
    fragment["sha256"] = sha256_hex(content);
    fragment["compiler"] = platform == "windows-amd64" ? "Microsoft (R) fake 19" : "g++ (fake) 13";
    fragment["flags"] = Json::parse(R"(["-O3"])");
    fragment["link_flags"] = Json::parse(R"(["-static"])");
    Json toolchain = Json::object();
    if (platform == "windows-amd64") {
        toolchain["cl"] = "Microsoft (R) fake 19";
        toolchain["tag"] = tag;
    } else {
        fragment["image"] = "dc_determinism-cpu:local";
        fragment["image_id"] = "sha256:img-" + platform + "-" + tag;
        toolchain["image"] = "dc_determinism-cpu:local";
        toolchain["image_id"] = "sha256:img-" + platform + "-" + tag;
    }
    fragment["toolchain"] = toolchain;
    if (with_built) fragment["built"] = folder + "/" + name;
    fs::path file = directory / "fragment.json";
    write_bytes(file, manifest_text(fragment));
    return path_to_utf8(file);
}

PrebuildOptions tool_options(const SyntheticRoot& synthetic, const std::vector<std::string>& fragments, bool check = false) {
    PrebuildOptions options;
    options.root = synthetic.root;
    options.tool_fragments = fragments;
    options.check = check;
    return options;
}

CheckRun run_tool_check(const PrebuildOptions& options) {
    CheckRun result;
    prebuild_hooks().report = [&result](const std::string& line) { result.lines.push_back(line); };
    try {
        result.code = run_prebuild(options);
    } catch (...) {
        prebuild_hooks().report = nullptr;
        throw;
    }
    prebuild_hooks().report = nullptr;
    return result;
}

// The source member the tool computes for the synthetic root.
Json expected_tool_source(const SyntheticRoot& synthetic) {
    Json vendor = read_json(synthetic.root / "src" / "vendor" / "manifest.json");
    Json dependencies = Json::object();
    for (const auto& item : {std::make_pair("nlohmann-json", "dependencies"), std::make_pair("miniz", "dc_determinism_dependencies")}) {
        Json entry = Json::object();
        entry["path"] = std::string("src/vendor/") + item.first;
        entry["pin"] = vendor[item.second][item.first]["pin"];
        entry["tree_sha256_lf"] = tree_sha256_lf(synthetic.root / "src" / "vendor" / item.first);
        dependencies[item.first] = entry;
    }
    Json source = Json::object();
    source["path"] = "src/dc_determinism";
    source["tree_sha256_lf"] = tree_sha256_lf(synthetic.root / "src" / "dc_determinism");
    source["dependencies"] = dependencies;
    return source;
}

}  // namespace

TEST(tool_fragments_become_the_dc_determinism_section) {
    SyntheticRoot synthetic;
    Json before = synthetic.read();
    ToolHost host(true);
    std::string windows = write_fragment(synthetic, "windows-amd64", "windows binary");
    std::string linux_fragment = write_fragment(synthetic, "linux-amd64", "linux binary");
    CHECK_EQ(run_prebuild(tool_options(synthetic, {windows, linux_fragment})), 0);

    Json manifest = synthetic.read();
    CHECK((keys_of(manifest) == std::vector<std::string>{"schema", "source", "binaries", "players", "dc_determinism"}));
    for (const char* member : {"schema", "source", "binaries", "players"}) CHECK_EQ(manifest[member], before[member]);
    const Json& section = manifest["dc_determinism"];
    CHECK((keys_of(section) == std::vector<std::string>{"source", "toolchain", "binaries"}));
    CHECK_EQ(section["source"], expected_tool_source(synthetic));
    CHECK((keys_of(section["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64"}));
    CHECK((keys_of(section["toolchain"]) == std::vector<std::string>{"windows-amd64", "linux-amd64"}));
    CHECK((keys_of(section["binaries"]["windows-amd64"]) == std::vector<std::string>{"path", "sha256", "compiler", "flags", "link_flags"}));
    CHECK((keys_of(section["binaries"]["linux-amd64"]) ==
           std::vector<std::string>{"path", "sha256", "compiler", "flags", "link_flags", "image", "image_id"}));
    CHECK_EQ(section["binaries"]["windows-amd64"]["sha256"], Json(sha256_hex("windows binary")));
    CHECK_EQ(section["binaries"]["linux-amd64"]["image_id"], Json("sha256:img-linux-amd64-1"));
    CHECK_EQ(section["toolchain"]["windows-amd64"]["tag"], Json("1"));
    CHECK_EQ(section["toolchain"]["linux-amd64"], Json::parse(R"({"image": "dc_determinism-cpu:local", "image_id": "sha256:img-linux-amd64-1"})"));
    // the built binaries are installed where the manifest says
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "dc_determinism" / "windows-amd64" / "dc_determinism.exe"), std::string("windows binary"));
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64" / "dc_determinism"), std::string("linux binary"));
#ifndef _WIN32
    CHECK((fs::status(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64" / "dc_determinism").permissions() & fs::perms::owner_exec) != fs::perms::none);
#endif
    CHECK_EQ(manifest_text(manifest), synthetic.text());
    // a fragment that is already installed (no `built`) is enough when its binary is there
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "linux binary", "1", false)})), 0);
}

TEST(the_tool_section_keeps_other_platforms_while_the_source_is_unchanged_and_the_windows_host_drops_them_when_it_changed) {
    SyntheticRoot synthetic;
    ToolHost host(true);
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w1"), write_fragment(synthetic, "linux-amd64", "l1")})), 0);
    // an arm64 entry of another host, with its file
    Json manifest = synthetic.read();
    Json arm64 = manifest["dc_determinism"]["binaries"]["linux-amd64"];
    arm64["path"] = "prebuilt/dc_determinism/linux-arm64/dc_determinism";
    arm64["sha256"] = sha256_hex("a1");
    manifest["dc_determinism"]["binaries"]["linux-arm64"] = arm64;
    manifest["dc_determinism"]["toolchain"]["linux-arm64"] = Json::parse(R"({"image": "arm", "image_id": "sha256:arm"})");
    synthetic.write_manifest(manifest);
    write_bytes(synthetic.root / "prebuilt" / "dc_determinism" / "linux-arm64" / "dc_determinism", "a1");

    // the same source: one platform is replaced, the others stay (in platform order)
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l2", "2")})), 0);
    Json kept = synthetic.read();
    const Json& section = kept["dc_determinism"];
    CHECK((keys_of(section["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64", "linux-arm64"}));
    CHECK_EQ(section["binaries"]["windows-amd64"]["sha256"], Json(sha256_hex("w1")));
    CHECK_EQ(section["binaries"]["linux-amd64"]["sha256"], Json(sha256_hex("l2")));
    CHECK_EQ(section["binaries"]["linux-arm64"], manifest["dc_determinism"]["binaries"]["linux-arm64"]);
    CHECK_EQ(section["toolchain"]["linux-amd64"]["image_id"], Json("sha256:img-linux-amd64-2"));
    CHECK_EQ(section["toolchain"]["linux-arm64"], manifest["dc_determinism"]["toolchain"]["linux-arm64"]);
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "dc_determinism" / "linux-arm64" / "dc_determinism"), std::string("a1"));

    // another source (src/dc_determinism changed): the Windows host keeps only what it rebuilt and removes the files of the rest
    write_bytes(synthetic.root / "src" / "dc_determinism" / "main.cpp", "int main() { return 1; }\n");
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w3", "3")})), 0);
    Json dropped = synthetic.read();
    CHECK((keys_of(dropped["dc_determinism"]["binaries"]) == std::vector<std::string>{"windows-amd64"}));
    CHECK((keys_of(dropped["dc_determinism"]["toolchain"]) == std::vector<std::string>{"windows-amd64"}));
    CHECK_EQ(dropped["dc_determinism"]["source"], expected_tool_source(synthetic));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64"));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "dc_determinism" / "linux-arm64"));
    CHECK_EQ(read_file(synthetic.root / "prebuilt" / "dc_determinism" / "windows-amd64" / "dc_determinism.exe"), std::string("w3"));
    CHECK_EQ(dropped["players"], kept["players"]);  // the other members are not touched

    // a changed dependency pin is another source too
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l4", "4")})), 0);  // same source: kept windows
    CHECK((keys_of(synthetic.read()["dc_determinism"]["binaries"]) == std::vector<std::string>{"windows-amd64", "linux-amd64"}));
    edit_manifest(synthetic, "src/vendor/manifest.json", [](Json& value) { value["dc_determinism_dependencies"]["miniz"]["pin"] = "other-pin"; });
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w5", "5")})), 0);
    CHECK((keys_of(synthetic.read()["dc_determinism"]["binaries"]) == std::vector<std::string>{"windows-amd64"}));
    CHECK(!fs::exists(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64"));
}

TEST(on_every_other_host_a_changed_tool_source_is_exit_2_and_nothing_is_written) {
    for (const char* host_name : {"Linux", "Darwin (through the image)"}) {
        SyntheticRoot synthetic;
        {
            ToolHost windows(true);
            CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w1"), write_fragment(synthetic, "linux-amd64", "l1")})), 0);
        }
        ToolHost host(false);
        // the same source: a Linux host updates its own platform
        CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l2", "2")})), 0);
        Json updated = synthetic.read();
        CHECK_EQ(updated["dc_determinism"]["binaries"]["windows-amd64"]["sha256"], Json(sha256_hex("w1")));
        CHECK_EQ(updated["dc_determinism"]["binaries"]["linux-amd64"]["sha256"], Json(sha256_hex("l2")));
        // the source changed: refused, the Windows host has to rebuild every platform
        write_bytes(synthetic.root / "src" / "dc_determinism" / "main.cpp", "int main() { return 2; }\n");
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l3", "3")})); });
        CHECK(message.find("the dc_determinism source (src/dc_determinism and its dependencies) differs from the section of prebuilt/manifest.json: "
                           "only the Windows host rebuilds every platform") == 0);
        CHECK(synthetic.snapshot() == before);
        CHECK_EQ(read_file(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64" / "dc_determinism"), std::string("l2"));
        // no section at all counts as another source
        Json manifest = synthetic.read();
        manifest.erase("dc_determinism");
        synthetic.write_manifest(manifest);
        CHECK(error_of([&] { run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l4", "4")})); }).find("(it has none)") != std::string::npos);
        (void)host_name;
    }
    // the mode needs no Docker: it is allowed in a container
    SyntheticRoot synthetic;
    FakeWorld world(synthetic.identity.worker_sha256);
    world.in_container = true;
    ToolHost host(true);
    CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w1")})), 0);
    CHECK(world.docker_calls.empty());
}

TEST(the_tool_check_compares_the_toolchain_first_then_every_binary_and_manifest_member) {
    SyntheticRoot synthetic;
    ToolHost host(true);
    std::string windows = write_fragment(synthetic, "windows-amd64", "w1");
    std::string linux_fragment = write_fragment(synthetic, "linux-amd64", "l1");
    CHECK_EQ(run_prebuild(tool_options(synthetic, {windows, linux_fragment})), 0);
    std::string manifest_before = synthetic.text();
    std::map<std::string, std::string> files = synthetic.snapshot();

    CheckRun same = run_tool_check(tool_options(synthetic, {windows, linux_fragment}, true));
    CHECK_EQ(same.code, 0);
    CHECK((same.lines == std::vector<std::string>{"binary windows-amd64: identical", "binary linux-amd64: identical",
                                                  "manifest dc_determinism.source: identical",
                                                  "manifest dc_determinism.binaries.windows-amd64: identical",
                                                  "manifest dc_determinism.binaries.linux-amd64: identical"}));
    CHECK_EQ(synthetic.text(), manifest_before);

    // another binary (the toolchain is the same): differs, with a line about the code sections, and the member is named
    std::string rebuilt = write_fragment(synthetic, "linux-amd64", "l2");
    CheckRun differs = run_tool_check(tool_options(synthetic, {windows, rebuilt}, true));
    CHECK_EQ(differs.code, 1);
    CHECK(contains(differs.lines, "binary windows-amd64: identical"));
    CHECK(has_line_starting(differs.lines, "binary linux-amd64: differs (installed " + sha256_hex("l1").substr(0, 12) + ", rebuilt " + sha256_hex("l2").substr(0, 12) + ")"));
    CHECK(contains(differs.lines, "binary linux-amd64: code sections not compared (not two PE or two ELF64 files)"));
    CHECK(contains(differs.lines, "manifest dc_determinism.binaries.linux-amd64: differs (dc_determinism.binaries.linux-amd64.sha256)"));
    CHECK(contains(differs.lines, "manifest dc_determinism.source: identical"));
    // two ELF files that differ only around their code: the code sections are identical
    write_bytes(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64" / "dc_determinism", make_elf("code", "note 1"));
    std::string elf = write_fragment(synthetic, "linux-amd64", make_elf("code", "note 2"));
    CheckRun elf_run = run_tool_check(tool_options(synthetic, {elf}, true));
    CHECK_EQ(elf_run.code, 1);
    CHECK(contains(elf_run.lines, "binary linux-amd64: code sections identical (.text)"));
    write_bytes(synthetic.root / "prebuilt" / "dc_determinism" / "linux-amd64" / "dc_determinism", "l1");
    // a binary that is not installed
    fs::remove(synthetic.root / "prebuilt" / "dc_determinism" / "windows-amd64" / "dc_determinism.exe");
    CheckRun missing = run_tool_check(tool_options(synthetic, {windows}, true));
    CHECK_EQ(missing.code, 1);
    CHECK(contains(missing.lines, "binary windows-amd64: differs (no installed binary at prebuilt/dc_determinism/windows-amd64)"));
    write_bytes(synthetic.root / "prebuilt" / "dc_determinism" / "windows-amd64" / "dc_determinism.exe", "w1");
    // another source (the tool changed since the section was written): the source member differs
    write_bytes(synthetic.root / "src" / "dc_determinism" / "main.cpp", "int main() { return 3; }\n");
    CheckRun source = run_tool_check(tool_options(synthetic, {windows}, true));
    CHECK_EQ(source.code, 1);
    CHECK(contains(source.lines, "manifest dc_determinism.source: differs (dc_determinism.source.tree_sha256_lf)"));
    write_bytes(synthetic.root / "src" / "dc_determinism" / "main.cpp", "int main() { return 0; }\n");

    // the toolchain first: nothing about the binaries is reported
    CheckRun other_toolchain = run_tool_check(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w9", "2"), linux_fragment}, true));
    CHECK_EQ(other_toolchain.code, 2);
    CHECK((other_toolchain.lines ==
           std::vector<std::string>{"toolchain differs: toolchain.windows-amd64.tag: recorded '1', current '2'",
                                    "dc_determinism check: the toolchain is not the recorded one, so the binaries were not rebuilt (exit code 2)"}));
    CheckRun other_image = run_tool_check(tool_options(synthetic, {write_fragment(synthetic, "linux-amd64", "l9", "9")}, true));
    CHECK_EQ(other_image.code, 2);
    CHECK(has_line_starting(other_image.lines, "toolchain differs: toolchain.linux-amd64.image_id: recorded 'sha256:img-linux-amd64-1', current 'sha256:img-linux-amd64-9'"));
    // a platform the section has no toolchain for, and a manifest without the section
    CheckRun arm64 = run_tool_check(tool_options(synthetic, {write_fragment(synthetic, "linux-arm64", "a1")}, true));
    CHECK_EQ(arm64.code, 2);
    CHECK(has_line_starting(arm64.lines, "toolchain not recorded: the dc_determinism section of prebuilt/manifest.json has no toolchain.linux-arm64"));
    Json manifest = synthetic.read();
    manifest.erase("dc_determinism");
    synthetic.write_manifest(manifest);
    CheckRun none = run_tool_check(tool_options(synthetic, {windows}, true));
    CHECK_EQ(none.code, 2);
    CHECK((none.lines == std::vector<std::string>{"the manifest has no dc_determinism section"}));
    // the check installs and writes nothing
    CHECK(files.size() >= 2);
}

TEST(a_tool_fragment_that_is_not_what_it_says_is_refused) {
    SyntheticRoot synthetic;
    ToolHost host(true);
    std::string good = write_fragment(synthetic, "linux-amd64", "l1");
    Json value = read_json(path_from_utf8(good));
    auto with = [&](const std::function<void(Json&)>& change) {
        Json copy = value;
        change(copy);
        fs::path file = synthetic.root / "build" / "fragments" / "broken.json";
        write_bytes(file, manifest_text(copy));
        return path_to_utf8(file);
    };
    std::map<std::string, std::string> before = synthetic.snapshot();
    auto refused = [&](const std::string& file, const std::string& fragment) {
        std::string message = error_of([&] { run_prebuild(tool_options(synthetic, {file})); });
        CHECK(message.find("tool fragment ") == 0 && message.find(fragment) != std::string::npos);
        CHECK(synthetic.snapshot() == before);
    };
    refused(with([](Json& v) { v["platform"] = "macos-arm64"; }), "unknown platform 'macos-arm64'");
    refused(with([](Json& v) { v["path"] = "prebuilt/dc_determinism/linux-arm64/dc_determinism"; }), "path is prebuilt/dc_determinism/linux-arm64/dc_determinism, expected prebuilt/dc_determinism/linux-amd64/dc_determinism");
    refused(with([](Json& v) { v.erase("sha256"); }), "no sha256");
    refused(with([](Json& v) { v.erase("link_flags"); }), "no link_flags list");
    refused(with([](Json& v) { v.erase("image_id"); }), "no image_id");
    refused(with([](Json& v) { v["toolchain"] = Json::object(); }), "no toolchain facts");
    refused(with([](Json& v) { v["built"] = "../outside"; }), "built must be relative to the repository root");
    refused(with([](Json& v) { v["built"] = "build/fragments/linux-amd64/missing"; }), "does not exist");
    refused(with([](Json& v) { v["sha256"] = std::string(64, '0'); }), "does not have the recorded sha256");
    CHECK(error_of([&] { run_prebuild(tool_options(synthetic, {path_to_utf8(synthetic.root / "build" / "fragments" / "nothing.json")})); })
              .find("cannot read ") != std::string::npos);
    // two fragments of one platform
    std::string message = error_of([&] { run_prebuild(tool_options(synthetic, {good, good})); });
    CHECK(message.find("two tool fragments for linux-amd64") == 0);
    // a manifest that is not this tool's, and sources that do not match their recorded trees
    edit_manifest(synthetic, "prebuilt/manifest.json", [](Json& v) { v["schema"] = "other-schema"; });
    CHECK(error_of([&] { run_prebuild(tool_options(synthetic, {good})); }).find("unexpected manifest schema: other-schema") == 0);
    edit_manifest(synthetic, "prebuilt/manifest.json", [](Json& v) { v["schema"] = "dc-determinism-prebuilt-v2"; });
    write_bytes(synthetic.root / "src" / "vendor" / "miniz" / "stray.txt", "x");
    CHECK(error_of([&] { run_prebuild(tool_options(synthetic, {good})); }).find("src/vendor/miniz does not match src/vendor/manifest.json (tree_sha256_lf ") == 0);
}

TEST(a_recorded_tool_section_key_that_is_no_platform_is_refused_and_never_removed) {
    for (const char* member : {"binaries", "toolchain"}) {
        SyntheticRoot synthetic;
        ToolHost host(true);
        CHECK_EQ(run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w1")})), 0);
        Json manifest = synthetic.read();
        manifest["dc_determinism"][member]["../src"] = Json::object();
        synthetic.write_manifest(manifest);
        write_bytes(synthetic.root / "prebuilt" / "dc_determinism" / "keep.txt", "x");
        write_bytes(synthetic.root / "src" / "dc_determinism" / "main.cpp", "int main() { return 9; }\n");  // another source
        std::map<std::string, std::string> before = synthetic.snapshot();
        std::string message = error_of([&] { run_prebuild(tool_options(synthetic, {write_fragment(synthetic, "windows-amd64", "w2", "2")})); });
        CHECK(message.find(std::string("has the unknown ") + member + " key '../src'") != std::string::npos);
        CHECK(synthetic.snapshot() == before);
        CHECK(fs::exists(synthetic.root / "src"));
    }
}

// --- code sections -------------------------------------------------------------------------------------------------------

TEST(code_sections_of_pe_and_elf_files_are_compared) {
    // The same code with other timestamps and data: the files differ, the code sections do not.
    std::string a = make_pe("code A", "data 1", 100);
    std::string b = make_pe("code A", "data 2 (other)", 200);
    CHECK(a != b);
    std::optional<ExecutableSections> sections = executable_sections(a);
    CHECK(sections.has_value() && sections->format == "PE" && sections->sections.size() == 1);
    CHECK_EQ(sections->sections[0].first, std::string(".text"));
    CHECK_EQ(sections->sections[0].second, sha256_hex("code A"));
    CodeComparison same = compare_code_sections(a, b);
    CHECK(same.comparable && same.identical && same.different.empty());
    CHECK_EQ(code_comparison_text(same), std::string("code sections identical (.text)"));
    CodeComparison other = compare_code_sections(a, make_pe("code B", "data 1", 100));
    CHECK(other.comparable && !other.identical);
    CHECK_EQ(code_comparison_text(other), std::string("code sections differ (.text)"));

    std::string c = make_elf("code A", "note 1");
    std::string d = make_elf("code A", "note 2");
    std::optional<ExecutableSections> elf = executable_sections(c);
    CHECK(elf.has_value() && elf->format == "ELF" && elf->sections.size() == 1);  // .note is not executable, .shstrtab neither
    CHECK(compare_code_sections(c, d).identical);
    CodeComparison elf_other = compare_code_sections(c, make_elf("code B!", "note 1"));
    CHECK(elf_other.comparable && !elf_other.identical);
    CHECK_EQ(code_comparison_text(elf_other), std::string("code sections differ (.text)"));

    // Anything else is not compared: other formats, a mix, damaged or truncated files.
    CHECK(!compare_code_sections("plain text", "plain text").comparable);
    CHECK_EQ(code_comparison_text(compare_code_sections("plain text", "plain text")),
             std::string("code sections not compared (not two PE or two ELF64 files)"));
    CHECK(!compare_code_sections(a, c).comparable);
    CHECK(!executable_sections(a.substr(0, 100)).has_value());
    CHECK(!executable_sections(c.substr(0, c.size() - 10)).has_value());
    std::string wrong_class = c;
    wrong_class[4] = 1;  // ELF32
    CHECK(!executable_sections(wrong_class).has_value());
    CHECK(!executable_sections(std::string()).has_value());
}

int main() { return testing::run_all_tests(); }
