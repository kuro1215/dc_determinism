#include "sim/wasm.hpp"

#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "player/binaries.hpp"
#include "process/process.hpp"

namespace dc_determinism {

const char* const kWasmCompiler = "wasm";
const char* const kWasmSchema = "dc-determinism-wasm-worker-v1";

namespace {

const char* const kTarget = "wasm32-emscripten";
const char* const kScriptName = "dc_worker.js";
const char* const kModuleName = "dc_worker.wasm";
const char* const kRebuildHint = "run `dc_determinism prebuild --wasm-worker-only`";

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// `directory in path.parents`, for paths that were both resolved from the same root.
bool below(const fs::path& path, const fs::path& directory) {
    fs::path relative = path.lexically_relative(directory);
    if (relative.empty() || relative == ".") return false;
    return *relative.begin() != "..";
}

// The member `keys[0].keys[1]...` of the manifest, or an "invalid wasm worker manifest" error.
Json member(const Json& manifest, const fs::path& path, const std::vector<const char*>& keys, bool hash = false) {
    Json value = manifest;
    std::string name;
    for (const char* key : keys) {
        name += (name.empty() ? "" : ".") + std::string(key);
        value = json_get(value, key);
    }
    bool ok = hash ? value.is_string() && is_hex64(value.get<std::string>()) : !value.is_null();
    if (!ok) throw DcDeterminismError("invalid wasm worker manifest: " + path_text(path) + " (missing or invalid " + name + ")");
    return value;
}

std::string text_member(const Json& manifest, const fs::path& path, const std::vector<const char*>& keys) {
    Json value = member(manifest, path, keys);
    if (!value.is_string()) {
        throw DcDeterminismError("invalid wasm worker manifest: " + path_text(path) + " (" + std::string(keys.back()) +
                         " is not a string)");
    }
    return value.get<std::string>();
}

}  // namespace

WasmBundle verify_wasm_bundle(const WasmOptions& options) {
    fs::path root = resolve(options.root.empty() ? repo_root() : options.root);
    fs::path manifest_path = root / "prebuilt" / "worker" / "wasm" / "manifest.json";
    if (!is_file(manifest_path)) {
        throw DcDeterminismError("wasm worker manifest not found: " + path_text(manifest_path) + " (" + kRebuildHint + ")");
    }
    Json manifest = read_json(manifest_path);
    if (json_get(manifest, "schema") != kWasmSchema) {
        throw DcDeterminismError("invalid wasm worker manifest: " + path_text(manifest_path) + " (schema is " +
                         py_repr(json_get(manifest, "schema")) + ")");
    }
    std::string manifest_sha = sha256_file(manifest_path);

    std::string script_text = text_member(manifest, manifest_path, {"worker", "path"});
    std::string script_sha = member(manifest, manifest_path, {"worker", "sha256"}, true).get<std::string>();
    std::string module_sha = member(manifest, manifest_path, {"worker", "sidecars", kModuleName}, true).get<std::string>();
    std::string source_sha = member(manifest, manifest_path, {"source", "sha256"}, true).get<std::string>();
    Json simulator_source = member(manifest, manifest_path, {"simulator", "source"});
    Json simulator_dependencies = member(manifest, manifest_path, {"simulator", "dependencies"});
    std::string compiler = text_member(manifest, manifest_path, {"toolchain", "emxx"});
    Json image_id = member(manifest, manifest_path, {"toolchain", "image_id"});
    Json override_sha = member(manifest, manifest_path, {"build", "override_sha256"}, true);
    Json shim_sha = member(manifest, manifest_path, {"build", "shim_sha256"});
    if (!simulator_source.is_object() || !simulator_dependencies.is_object()) {
        throw DcDeterminismError("invalid wasm worker manifest: " + path_text(manifest_path) +
                         " (simulator.source and simulator.dependencies must be objects)");
    }

    fs::path relative = path_from_utf8(script_text);
    fs::path script = resolve(root / relative);
    bool unsafe = relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
                  !below(script, resolve(root / "prebuilt")) || script.filename() != kScriptName;
    if (unsafe) throw DcDeterminismError("unsafe wasm worker path in the manifest: " + script_text);
    fs::path module = script.parent_path() / kModuleName;

    if (!is_file(script)) throw DcDeterminismError("wasm worker not found: " + path_text(script));
    if (!is_file(module)) throw DcDeterminismError("wasm module not found: " + path_text(module));
    std::string actual_script = sha256_file(script);
    if (actual_script != script_sha) {
        throw DcDeterminismError("wasm worker hash mismatch: " + path_text(script) + " is " + actual_script +
                         ", manifest says " + script_sha);
    }
    std::string actual_module = sha256_file(module);
    if (actual_module != module_sha) {
        throw DcDeterminismError("wasm module hash mismatch: " + path_text(module) + " is " + actual_module +
                         ", manifest says " + module_sha);
    }

    // The bundle must come from the worker source and the simulator sources of this repository.
    Json native = worker_manifest(root);
    std::string native_source = native["source"]["sha256"].get<std::string>();
    if (native_source != source_sha) {
        throw DcDeterminismError("stale wasm bundle: built from worker source " + source_sha +
                         ", but prebuilt/manifest.json records " + native_source + " (" + kRebuildHint + ")");
    }
    fs::path worker_source = root / "src" / "worker" / "worker.cpp";
    if (is_file(worker_source)) {
        std::string current = sha256_lf_file(worker_source);
        if (current != source_sha) {
            throw DcDeterminismError("stale wasm bundle: built from worker source " + source_sha +
                             ", but src/worker/worker.cpp is " + current + " (" + kRebuildHint + ")");
        }
    }
    fs::path loader_manifest_path = root / "prebuilt" / "loaders" / platform_key() / "manifest.json";
    if (!is_file(loader_manifest_path)) {
        throw DcDeterminismError("loader manifest not found: " + path_text(loader_manifest_path));
    }
    Json loader_manifest = read_json(loader_manifest_path);
    if (json_get(loader_manifest, "source") != simulator_source ||
        json_get(loader_manifest, "dependencies") != simulator_dependencies) {
        throw DcDeterminismError("wasm simulator source mismatch: the bundle was built from other simulator sources or "
                         "dependencies than " + path_text(loader_manifest_path) + " records (" + kRebuildHint + ")");
    }

    WasmBundle bundle;
    Json runtime = Json::object();
    std::string version;
    if (options.check_node) {
        std::string node = options.node;
        if (node.empty()) {
            std::optional<fs::path> found = find_on_path("node");
            if (!found) throw DcDeterminismError("the wasm worker requires node on PATH");
            node = path_text(*found);
        }
        bundle.command = {node, path_text(script)};
        version = worker_version(bundle.command);
        std::string expected = "dc_worker " + source_sha;
        if (version != expected) {
            throw DcDeterminismError("wasm worker version mismatch: " + py_repr(Json(version)) + " != " +
                             py_repr(Json(expected)));
        }
        runtime["node"] = node_version(node);
    }

    // The manifest entry: what the native `manifest_entry` carries (path, hash, compiler, flags, image).
    Json entry = member(manifest, manifest_path, {"worker"});
    entry["compiler"] = compiler;
    entry["flags"] = json_get(json_get(manifest, "flags"), "link");
    entry["image"] = json_get(json_get(manifest, "toolchain"), "image");
    entry["image_id"] = image_id;
    entry["manifest"] = path_text(manifest_path);
    entry["manifest_sha256"] = manifest_sha;

    Json worker = Json::object();
    worker["path"] = path_text(script);
    worker["sha256"] = script_sha;
    worker["version"] = options.check_node ? Json(version) : Json(nullptr);
    worker["manifest_entry"] = entry;
    worker["runtime"] = runtime;
    bundle.worker = worker;

    Json library = Json::object();
    library["filename"] = kModuleName;
    library["sha256"] = module_sha;
    library["sidecars"] = Json::object();
    library["compiler_version"] = compiler;
    library["target"] = kTarget;
    Json wasm_build = Json::object();
    wasm_build["override_sha256"] = override_sha;
    wasm_build["shim_sha256"] = shim_sha;
    wasm_build["manifest_sha256"] = manifest_sha;
    Json prebuilt = Json::object();
    prebuilt["source"] = simulator_source;
    prebuilt["dependencies"] = simulator_dependencies;
    prebuilt["image_id"] = image_id;
    prebuilt["compiler"] = kWasmCompiler;
    prebuilt["library"] = library;
    prebuilt["wasm_build"] = wasm_build;
    Json info = Json::object();
    info["path"] = path_text(module);
    info["sha256"] = module_sha;
    info["compiler"] = kWasmCompiler;
    info["platform"] = kTarget;
    info["origin"] = "prebuilt";
    info["dc_prebuilt"] = prebuilt;
    bundle.loader = ResolvedLoader{module, info};
    return bundle;
}

}  // namespace dc_determinism
