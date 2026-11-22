#include "sim/provenance.hpp"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "process/process.hpp"
#include "sim/compilers.hpp"

#ifndef _WIN32
#include <unistd.h>
#endif

namespace dc_determinism {

namespace {

const char* const kWorkerSchema = "dc-determinism-prebuilt-v2";
const char* const kLinuxLoader = "libdigitalcurling_plugin_loader.so";

const char* dc_schema() {
    if (is_windows()) return "dc-prebuilt-windows-amd64-v1";
    return std::string(platform_key()) == "linux-arm64" ? "dc-prebuilt-linux-arm64-v1" : "dc-prebuilt-linux-amd64-v1";
}

void require_host() {
#if defined(__x86_64__) || defined(_M_X64) || (defined(__linux__) && defined(__aarch64__))
    return;
#else
    throw DcDeterminismError("unsupported host: requires x86-64 or Linux aarch64");
#endif
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// `{value}` of a manifest value inside a message (`None` for a missing one).
std::string value_text(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    return py_repr(value);
}

// The (possibly falsy) member as an object: `record.get(key) or {}`.
Json object_or_empty(const Json& value) {
    return py_truthy(value) ? value : Json::object();
}

std::string file_sha256(const fs::path& path) { return sha256_file(path); }

}  // namespace

const char* platform_key() {
    if (is_windows()) return "windows-amd64";
#if defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#else
    return "linux-amd64";
#endif
}

const char* worker_file_name() { return is_windows() ? "dc_worker.exe" : "dc_worker"; }

fs::path loaders_root() { return repo_root() / "prebuilt" / "loaders"; }

ResolvedLoader verify_dc_loader(const std::string& compiler) {
    require_host();
    if (compiler == "msvc" && !is_windows()) throw DcDeterminismError("--compiler msvc is available on Windows only");
    require_compiler_platform(compiler, platform_key());
    fs::path bundle = loaders_root() / platform_key();
    fs::path manifest_path = bundle / "manifest.json";
    Json manifest = read_json(manifest_path);
    Json schema = json_get(manifest, "schema");
    if (schema != Json(dc_schema())) {
        throw DcDeterminismError("unexpected loader manifest schema in " + path_text(manifest_path) + ": " +
                         py_repr(schema));
    }
    Json record = json_get(json_get(manifest, "libraries"), compiler.c_str());
    if (!record.is_object()) {
        throw DcDeterminismError("compiler " + py_repr(Json(compiler)) + " is not in " + path_text(manifest_path));
    }
    Json filename_value = json_get(record, "filename", Json(kLinuxLoader));
    std::string filename = filename_value.is_string() ? filename_value.get<std::string>() : py_repr(filename_value);
    fs::path directory = bundle / path_from_utf8(compiler);
    fs::path path = directory / path_from_utf8(filename);
    if (!is_file(path)) throw DcDeterminismError("loader not found: " + path_text(path));
    std::string digest = file_sha256(path);
    Json recorded = json_get(record, "sha256");
    if (recorded != Json(digest)) throw DcDeterminismError(compiler + " loader hash mismatch: " + path_text(path));

    Json sidecars = object_or_empty(json_get(record, "sidecars"));
    std::set<std::string> expected_names{filename};
    for (auto it = sidecars.begin(); it != sidecars.end(); ++it) {
        const std::string& name = it.key();
        expected_names.insert(name);
        fs::path name_path = path_from_utf8(name);
        if (name_path.filename() != name_path || !is_file(directory / name_path) ||
            Json(file_sha256(directory / name_path)) != it.value()) {
            throw DcDeterminismError("runtime sidecar hash mismatch: " + name);
        }
    }
    if (is_windows()) {
        std::set<std::string> actual;
        std::error_code ec;
        for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
            std::string name = path_to_utf8(it->path().filename());
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0) actual.insert(name);
        }
        if (actual != expected_names) {
            throw DcDeterminismError("loader directory contains unrecorded or missing runtime DLLs");
        }
    }
    Json build = object_or_empty(json_get(record, "build"));
    Json library = Json::object();
    library["filename"] = filename;
    library["sha256"] = record["sha256"];
    library["sidecars"] = sidecars;
    library["compiler_version"] = json_get(build, "compiler_version", json_get(record, "compiler_version"));
    library["target"] = json_get(build, "target", json_get(record, "target"));
    Json info = Json::object();
    info["source"] = json_get(manifest, "source");
    info["dependencies"] = json_get(manifest, "dependencies");
    info["image_id"] = json_get(manifest, "image_id");
    info["compiler"] = compiler;
    info["library"] = library;
    return ResolvedLoader{path, info};
}

ResolvedLoader loader_provenance(const std::string& compiler) {
    if (compiler.empty()) throw DcDeterminismError("--compiler is required");
    ResolvedLoader prebuilt = verify_dc_loader(compiler);
    Json info = Json::object();
    info["path"] = path_text(prebuilt.path);
    info["sha256"] = prebuilt.info["library"]["sha256"];
    info["compiler"] = compiler;
    info["platform"] = platform_key();
    info["origin"] = "prebuilt";
    info["dc_prebuilt"] = prebuilt.info;
    return ResolvedLoader{prebuilt.path, info};
}

fs::path default_worker_path() {
    return resolve(repo_root() / "prebuilt" / "worker" / platform_key() / worker_file_name());
}

namespace {

// The command prefix as shown in messages: `path` for the native worker, `node dc_worker.js` for wasm.
std::string prefix_text(const std::vector<std::string>& prefix) {
    std::string text;
    for (const std::string& part : prefix) text += (text.empty() ? "" : " ") + part;
    return text;
}

// `subprocess.run([*prefix, flag], capture_output=True, text=True, timeout=...)`.
RunResult run_worker_flag(const std::vector<std::string>& prefix, const char* flag, double timeout_seconds) {
    StartOptions options;
    options.argv = prefix;
    options.argv.push_back(flag);
    RunResult result;
    try {
        result = run_capture(options, std::string(), timeout_seconds);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError("cannot run " + prefix_text(prefix) + " " + flag + ": " + error.what());
    }
    if (result.timed_out) {
        std::string command = "[";
        for (const std::string& part : prefix) command += "'" + part + "', ";
        throw DcDeterminismError("cannot run " + prefix_text(prefix) + " " + flag + ": Command '" + command + "'" + flag +
                         "']' timed out after " + format_g(timeout_seconds) + " seconds");
    }
    return result;
}

}  // namespace

std::string worker_version(const std::vector<std::string>& prefix, double timeout_seconds) {
    RunResult result = run_worker_flag(prefix, "--version", timeout_seconds);
    if (result.exit_code != 0) {
        std::string detail = py_strip(result.err);
        if (detail.empty()) detail = py_strip(result.out);
        throw DcDeterminismError(prefix_text(prefix) + " --version exited with code " + std::to_string(result.exit_code) +
                         ": " + detail);
    }
    return py_strip(result.out);
}

void worker_self_test(const std::vector<std::string>& prefix, double timeout_seconds) {
    RunResult result = run_worker_flag(prefix, "--self-test", timeout_seconds);
    if (result.exit_code != 0 || py_strip(result.out) != "OK") {
        throw DcDeterminismError("worker self-test failed (exit " + std::to_string(result.exit_code) +
                         "): " + py_strip(result.out + result.err));
    }
}

std::string worker_version(const fs::path& path, double timeout_seconds) {
    return worker_version(std::vector<std::string>{path_text(path)}, timeout_seconds);
}

void worker_self_test(const fs::path& path, double timeout_seconds) {
    worker_self_test(std::vector<std::string>{path_text(path)}, timeout_seconds);
}

void check_executable(const fs::path& path) {
    if (!is_file(path)) throw DcDeterminismError("worker executable not found: " + path_text(path));
#ifndef _WIN32
    if (access(path.c_str(), X_OK) != 0) {
        throw DcDeterminismError("worker is not executable: " + path_text(path) + " (try: chmod +x " + path_text(path) + ")");
    }
#endif
}

Json worker_manifest(const fs::path& root) {
    fs::path path = (root.empty() ? repo_root() : root) / "prebuilt" / "manifest.json";
    if (!is_file(path)) {
        throw DcDeterminismError("worker manifest not found: " + path_text(path) + " (run `dc_determinism prebuild` first)");
    }
    Json manifest = read_json(path);
    Json source = json_get(object_or_empty(json_get(manifest, "source")), "sha256");
    bool schema_ok = json_get(manifest, "schema") == kWorkerSchema;
    if (!schema_ok || !source.is_string() || !is_hex64(source.get<std::string>())) {
        throw DcDeterminismError("invalid worker manifest: " + path_text(path));
    }
    return manifest;
}

Json verify_default_worker() {
    fs::path path = default_worker_path();
    if (!is_file(path)) throw DcDeterminismError("worker executable not found: " + path_text(path));
#ifndef _WIN32
    if (access(path.c_str(), X_OK) != 0) {
        throw DcDeterminismError("worker is not executable: " + path_text(path) + " (try: chmod +x " + path_text(path) + ")");
    }
#endif
    Json manifest = worker_manifest();
    Json entry = json_get(object_or_empty(json_get(manifest, "binaries")), platform_key());
    if (!entry.is_object()) {
        throw DcDeterminismError(std::string("no ") + platform_key() + " entry in the worker manifest");
    }
    std::string digest = file_sha256(path);
    if (Json(digest) != json_get(entry, "sha256")) {
        throw DcDeterminismError("worker hash mismatch: " + path_text(path) + " is " + digest + ", manifest says " +
                         value_text(json_get(entry, "sha256")));
    }
    std::string version = worker_version(path);
    std::string expected = "dc_worker " + manifest["source"]["sha256"].get<std::string>();
    if (version != expected) {
        throw DcDeterminismError("worker version mismatch: " + py_repr(Json(version)) + " != " + py_repr(Json(expected)));
    }
    Json out = Json::object();
    out["path"] = path_text(path);
    out["sha256"] = digest;
    out["version"] = version;
    out["manifest_entry"] = entry;
    return out;
}

// --- loader identity (`compare simulator`) --------------------------------------------------------

namespace {

// The `tree_sha256_lf` of a loader's `source` member (null when it records none).
Json simulator_tree(const Json& source) {
    Json found = json_get(source, "tree_sha256_lf");
    return found.is_string() ? found : Json(nullptr);
}

}  // namespace

Json source_identity(const Json& loader) {
    Json origin = json_get(loader, "origin");
    if (origin != Json("prebuilt")) {
        throw DcDeterminismError("incompatible results: the loader origin is " + py_repr(origin) +
                         ", but only \"prebuilt\" loaders are supported");
    }
    Json prebuilt = object_or_empty(json_get(loader, "dc_prebuilt"));
    Json detail = Json::object();
    detail["source"] = json_get(prebuilt, "source");
    detail["dependencies"] = json_get(prebuilt, "dependencies");
    Json out = Json::object();
    out["origin"] = origin;
    out["simulator_tree"] = simulator_tree(json_get(prebuilt, "source"));
    out["detail"] = detail;
    return out;
}

Json check_source_identity(const Json& left, const Json& right) {
    Json a = source_identity(left);
    Json b = source_identity(right);
    if (a["detail"] != b["detail"]) {
        throw DcDeterminismError("incompatible results: the loaders were built from different simulator sources or "
                         "dependencies");
    }
    Json side_a = Json::object();
    side_a["origin"] = a["origin"];
    side_a["simulator_tree"] = a["simulator_tree"];
    Json side_b = Json::object();
    side_b["origin"] = b["origin"];
    side_b["simulator_tree"] = b["simulator_tree"];
    Json out = Json::object();
    out["a"] = side_a;
    out["b"] = side_b;
    out["verified"] = true;
    return out;
}

}  // namespace dc_determinism
