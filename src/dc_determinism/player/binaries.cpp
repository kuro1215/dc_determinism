#include "player/binaries.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "process/process.hpp"
#include "sim/compilers.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {

const char* const kPlayerCompareSchema = "dc-determinism-player-comparison-v2";
const char* const kPlayerRevision = "762eef67560cd83b23ef3afd272ac45e1ff28d7b";

namespace {

struct SidecarSpec {
    std::string name;
    fs::path path;
    std::string digest;
};

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// A path in the form Python compares `Path` objects: normalised, case-folded on Windows.
std::string comparable(const fs::path& path) {
    std::string text = path_to_utf8(path.lexically_normal());
    while (text.size() > 1 && (text.back() == '/' || text.back() == '\\')) text.pop_back();
    if (is_windows()) {
        for (char& c : text) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

// `directory in path.parents`.
bool below(const fs::path& path, const fs::path& directory) {
    std::string wanted = comparable(directory);
    fs::path current = path;
    for (;;) {
        fs::path parent = current.parent_path();
        if (parent == current || parent.empty()) return false;
        if (comparable(parent) == wanted) return true;
        current = parent;
    }
}

// `relative.is_absolute() or ".." in relative.parts`.
bool unsafe_relative(const fs::path& relative) {
    if (relative.is_absolute() || relative.has_root_name() || relative.has_root_directory()) return true;
    for (const fs::path& part : relative) {
        if (part == "..") return true;
    }
    return false;
}

// `str(value)` of a decoded JSON value inside a message.
std::string value_text(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    return py_repr(value);
}

fs::path effective_root(const fs::path& root) { return resolve(root.empty() ? repo_root() : root); }

std::vector<SidecarSpec> sidecar_specs(const Json& entry, const fs::path& root, const fs::path& binary) {
    std::vector<SidecarSpec> specs;
    Json sidecars = json_get(entry, "sidecars");
    if (!py_truthy(sidecars)) return specs;
    if (!sidecars.is_object()) throw DcDeterminismError("invalid sidecars in the manifest: " + py_repr(sidecars));
    for (auto it = sidecars.begin(); it != sidecars.end(); ++it) {
        const std::string& name = it.key();
        const Json& value = it.value();
        fs::path name_path = path_from_utf8(name);
        if (name_path.filename() != name_path) {
            throw DcDeterminismError("unsafe sidecar name in the manifest: " + py_repr(Json(name)));
        }
        SidecarSpec spec;
        spec.name = name;
        Json digest;
        if (value.is_object()) {
            Json path_value = json_get(value, "path");
            fs::path relative = path_from_utf8(path_value.is_null() ? std::string("None") : value_text(path_value));
            fs::path path = resolve(root / relative);
            if (unsafe_relative(relative) || !below(path, resolve(root / "prebuilt"))) {
                throw DcDeterminismError("unsafe sidecar path in the manifest: " + py_repr(path_value));
            }
            spec.path = path;
            digest = json_get(value, "sha256");
        } else {
            spec.path = binary.parent_path() / name_path;
            digest = value;
        }
        if (!digest.is_string()) throw DcDeterminismError("invalid sidecar hash in the manifest: " + py_repr(Json(name)));
        spec.digest = digest.get<std::string>();
        specs.push_back(spec);
    }
    return specs;
}

// `file_hashes`: the binary's hash and the hashes of its sidecars, by name.
std::pair<std::string, std::map<std::string, std::string>> file_hashes(
    const fs::path& path, const std::vector<std::string>& names, const std::map<std::string, fs::path>& paths) {
    std::string digest = sha256_file(path);
    std::map<std::string, std::string> sidecars;
    for (const std::string& name : names) {
        auto found = paths.find(name);
        sidecars[name] = sha256_file(found != paths.end() && !found->second.empty() ? found->second
                                                                                   : path.parent_path() / path_from_utf8(name));
    }
    return {digest, sidecars};
}

}  // namespace

std::string player_binary_key(const std::string& player, const std::string& compiler) {
    if (compiler == "wasm") return "wasm/" + player;
    if (compiler == "msvc" && !is_windows()) throw DcDeterminismError("--compiler msvc is available on Windows only");
    require_compiler_platform(compiler, platform_key());
    return std::string(platform_key()) + "-" + compiler + "/" + player;
}

Json players_section(const fs::path& root) {
    Json manifest = worker_manifest(root);
    Json section = json_get(manifest, "players");
    if (!section.is_object() || !json_get(section, "binaries").is_object()) {
        throw DcDeterminismError("the prebuilt manifest has no `players` section (run `dc_determinism prebuild`)");
    }
    return section;
}

Json verify_player_binary(const std::string& key, const fs::path& root_given) {
    fs::path root = effective_root(root_given);
    Json section = players_section(root);
    Json entry = json_get(section["binaries"], key.c_str());
    if (!entry.is_object() || !json_get(entry, "path").is_string()) {
        throw DcDeterminismError("no prebuilt player binary " + py_repr(Json(key)) +
                         " in the manifest (not built for this platform?)");
    }
    std::string entry_path = entry["path"].get<std::string>();
    fs::path relative = path_from_utf8(entry_path);
    fs::path path = resolve(root / relative);
    if (unsafe_relative(relative) || !below(path, root)) {
        throw DcDeterminismError("unsafe player path in the manifest: " + entry_path);
    }
    std::vector<SidecarSpec> specs = sidecar_specs(entry, root, path);
    std::vector<std::string> names;
    std::map<std::string, fs::path> paths;
    std::map<std::string, std::string> expected;
    for (const SidecarSpec& spec : specs) {
        names.push_back(spec.name);
        paths[spec.name] = spec.path;
        expected[spec.name] = spec.digest;
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    if (path.extension() == ".js") {
        if (!is_file(path)) throw DcDeterminismError("player binary not found: " + path_text(path));
    } else {
        check_executable(path);
    }
    for (const SidecarSpec& spec : specs) {
        if (!is_file(spec.path)) throw DcDeterminismError("player sidecar not found: " + path_text(spec.path));
    }
    std::pair<std::string, std::map<std::string, std::string>> hashes = file_hashes(path, names, paths);
    if (Json(hashes.first) != json_get(entry, "sha256")) {
        throw DcDeterminismError("player hash mismatch: " + path_text(path) + " is " + hashes.first + ", manifest says " +
                         value_text(json_get(entry, "sha256")));
    }
    for (const auto& item : hashes.second) {
        if (item.second != expected[item.first]) throw DcDeterminismError("player sidecar hash mismatch: " + item.first);
    }

    Json info = Json::object();
    info["key"] = key;
    info["path"] = path_text(path);
    info["sha256"] = hashes.first;
    Json side = Json::object();
    for (const auto& item : hashes.second) side[item.first] = item.second;
    info["sidecars"] = side;
    Json side_paths = Json::object();
    for (const SidecarSpec& spec : specs) side_paths[spec.name] = path_text(spec.path);
    info["sidecar_paths"] = side_paths;
    info["compiler"] = json_get(entry, "compiler");
    info["flags"] = json_get(entry, "flags");
    info["manifest_entry"] = entry;
    return info;
}

Json describe_explicit_player(const fs::path& path, const std::string& key, const fs::path& root_given) {
    if (!is_file(path)) throw DcDeterminismError("player binary not found: " + path_text(path));
    bool script = path.extension() == ".js";
    if (!script) check_executable(path);
    fs::path wasm = path;
    wasm.replace_extension(".wasm");
    std::map<std::string, std::string> sidecars;
    if (script && is_file(wasm)) sidecars[path_to_utf8(wasm.filename())] = sha256_file(wasm);
    std::string digest = sha256_file(path);
    Json entry = nullptr;
    try {
        Json candidate = json_get(players_section(root_given)["binaries"], key.c_str());
        std::map<std::string, std::string> expected;
        bool known = candidate.is_object();
        if (known) {
            for (const SidecarSpec& spec : sidecar_specs(candidate, effective_root(root_given), path)) {
                expected[spec.name] = spec.digest;
            }
        }
        if (known && json_get(candidate, "sha256") == Json(digest) && expected == sidecars) entry = candidate;
    } catch (const DcDeterminismError&) {
    }
    Json info = Json::object();
    info["key"] = key;
    info["path"] = path_text(path);
    info["sha256"] = digest;
    Json side = Json::object();
    for (const auto& item : sidecars) side[item.first] = item.second;
    info["sidecars"] = side;
    info["compiler"] = json_get(entry, "compiler");
    info["flags"] = json_get(entry, "flags");
    info["manifest_entry"] = entry;
    return info;
}

void recheck_binary(const Json& info) {
    fs::path path = path_from_utf8(info["path"].get<std::string>());
    std::vector<std::string> names;
    std::map<std::string, std::string> recorded;
    Json sidecars = json_get(info, "sidecars");
    if (sidecars.is_object()) {
        for (auto it = sidecars.begin(); it != sidecars.end(); ++it) {
            names.push_back(it.key());
            recorded[it.key()] = it.value().get<std::string>();
        }
    }
    std::sort(names.begin(), names.end());
    std::map<std::string, fs::path> paths;
    Json sidecar_paths = json_get(info, "sidecar_paths");
    if (sidecar_paths.is_object()) {
        for (auto it = sidecar_paths.begin(); it != sidecar_paths.end(); ++it) {
            paths[it.key()] = path_from_utf8(it.value().get<std::string>());
        }
    }
    std::pair<std::string, std::map<std::string, std::string>> hashes = file_hashes(path, names, paths);
    if (Json(hashes.first) != info["sha256"] || hashes.second != recorded) {
        throw DcDeterminismError("player binary changed between repetitions");
    }
}

std::vector<std::string> parse_player_command(const std::string& text) {
    Json prefix = Json::parse(text, nullptr, false);
    if (prefix.is_discarded()) throw DcDeterminismError("--player-command must be a JSON array: invalid JSON");
    bool ok = prefix.is_array() && !prefix.empty();
    if (ok) {
        for (const Json& item : prefix) ok = ok && item.is_string();
    }
    if (!ok) throw DcDeterminismError("--player-command must be a non-empty JSON array of strings");
    return prefix.get<std::vector<std::string>>();
}

Json node_version(const std::string& node) {
    StartOptions options;
    options.argv = {node, "--version"};
    try {
        RunResult result = run_capture(options, std::string(), 60.0);
        if (result.timed_out || result.exit_code != 0) return nullptr;
        return py_strip(result.out);
    } catch (const DcDeterminismError&) {
        return nullptr;
    }
}

PlayerSelection resolve_player(const std::string& player, const std::string& compiler,
                               const std::string& player_command) {
    std::string key = player_binary_key(player, compiler);
    PlayerSelection selection;
    selection.runtime = Json::object();
    if (!player_command.empty()) {
        // Test hook: a command prefix replaces the binary; nothing is verified.
        selection.command = parse_player_command(player_command);
        Json info = Json::object();
        info["key"] = key;
        info["path"] = nullptr;
        info["sha256"] = nullptr;
        info["sidecars"] = Json::object();
        info["compiler"] = nullptr;
        info["flags"] = nullptr;
        info["manifest_entry"] = nullptr;
        info["command"] = selection.command;
        selection.binary = info;
        return selection;
    }
    Json info = verify_player_binary(key);
    info["origin"] = "prebuilt";
    std::string path = info["path"].get<std::string>();
    selection.binary = info;
    if (compiler != "wasm") {
        selection.command = {path};
        return selection;
    }
    std::optional<fs::path> node = find_on_path("node");
    if (!node) throw DcDeterminismError("wasm players require node on PATH");
    std::string node_text = path_text(*node);
    selection.command = {node_text, path};
    selection.runtime["node"] = node_version(node_text);
    return selection;
}

}  // namespace dc_determinism
