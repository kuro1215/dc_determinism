#include "localbuild/local_build.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>
#include <utility>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/tempdir.hpp"
#include "common/text.hpp"
#include "common/tree_hash.hpp"
#include "corpus/pytext.hpp"
#include "json/writer.hpp"
#include "process/process.hpp"
#include "sim/compilers.hpp"

#ifndef _WIN32
#include <sys/utsname.h>
#endif

namespace dc_determinism {

const char* const kBuildReceiptSchema = "dc-determinism-build-receipt-v1";
const char* const kBuildReceiptName = "build-receipt.json";

namespace {

constexpr int kRecipe = 1;
constexpr double kProbeScriptTimeout = 600.0;

const char* const kSimulatorRel = "src/vendor/digitalcurling";
const char* const kDepsRel = "src/vendor";
const char* const kManifestRel = "src/vendor/manifest.json";
const char* const kPlayerLibRel = "src/vendor/digitalcurling";
const char* const kPlayerWorkerRel = "src/worker/player.cpp";

// FetchContent source-override variable -> vendored dependency directory name.
const std::pair<const char*, const char*> kLoaderDependencies[] = {
    {"FPM", "fpm"}, {"NLOHMANN_JSON", "nlohmann-json"}, {"BOX2D", "box2d"}, {"XXHASH", "xxhash"},
};
const char* const kPlayerDependencies[] = {"nlohmann-json", "fpm"};
const char* const kPlayers[] = {"normal_dist", "nd2"};
const char* const kMingwRuntimeDlls[] = {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll",
                                         "libc++.dll", "libunwind.dll"};

template <std::size_t N>
bool contains(const char* const (&list)[N], const std::string& value) {
    for (const char* item : list) {
        if (value == item) return true;
    }
    return false;
}

std::string joined(const char* const* items, std::size_t count, const char* separator) {
    std::string out;
    for (std::size_t i = 0; i < count; ++i) out += (i == 0 ? "" : separator) + std::string(items[i]);
    return out;
}

bool is_file(const fs::path& path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

bool directory_exists(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

// `str(Path)`: the platform's separators.
std::string py_str(fs::path path) {
    path.make_preferred();
    return path_to_utf8(path);
}

// `posix(path)`: `str(path)` with forward slashes.
std::string posix(const std::string& text) {
    std::string out = text;
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::string posix(const fs::path& path) { return posix(py_str(path)); }

// `root / "a/b/c"`.
fs::path rel(const fs::path& root, const std::string& relative) { return root / path_from_utf8(relative); }

// `errors="replace"`: invalid UTF-8 bytes become U+FFFD.
std::string scrub_utf8(const std::string& bytes) {
    if (is_valid_utf8(bytes)) return bytes;
    std::string out;
    std::size_t i = 0;
    while (i < bytes.size()) {
        unsigned char lead = static_cast<unsigned char>(bytes[i]);
        std::size_t length = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 0;
        if (length > 0 && i + length <= bytes.size() && is_valid_utf8(bytes.substr(i, length))) {
            out.append(bytes, i, length);
            i += length;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out;
}

Json required_member(const Json& object, const char* key, const std::string& where) {
    Json value = json_get(object, key);
    if (value.is_null() && !(object.is_object() && object.contains(key))) {
        throw DcDeterminismError(where + " has no " + std::string(key));
    }
    return value;
}

// --- build script ------------------------------------------------------------------------------------

// The toolchain script exists for Windows hosts only; the Linux and wasm builds of `prebuild` run in Docker.
fs::path build_script(const fs::path& root) {
    if (!is_windows()) throw DcDeterminismError("the local toolchain script (scripts/build-local.ps1) is available on Windows only");
    fs::path script = root / "scripts" / "build-local.ps1";
    if (!is_file(script)) throw DcDeterminismError("the build script is missing: " + py_str(script));
    return script;
}

std::vector<std::string> script_command(const fs::path& root, const std::vector<std::string>& arguments) {
    fs::path script = build_script(root);
    std::string powershell = "powershell.exe";
    std::optional<std::string> system_root = env_get("SystemRoot");
    if (system_root && !system_root->empty()) {
        fs::path candidate = path_from_utf8(*system_root) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe";
        if (is_file(candidate)) powershell = path_to_utf8(candidate);
    }
    std::vector<std::string> argv = {powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                     "-File",    path_to_utf8(script)};
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    return argv;
}

std::string last_lines(const std::string& text, std::size_t count) {
    std::vector<std::string> lines = py_splitlines(py_strip(text));
    std::size_t start = lines.size() > count ? lines.size() - count : 0;
    std::string out;
    for (std::size_t i = start; i < lines.size(); ++i) out += (i == start ? "" : "\n") + lines[i];
    return out;
}

// `probe <kind> <compiler>` of the build script: the toolchain description or the DcDeterminismError.
Json script_probe(const fs::path& root, const std::string& kind, const std::string& compiler) {
    std::vector<std::string> arguments = {"-Action", "probe", "-Kind", kind, "-Compiler", compiler};
    StartOptions options;
    options.argv = script_command(root, arguments);
    RunResult result;
    try {
        result = run_capture(options, std::string(), kProbeScriptTimeout);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("cannot run the build script: ") + error.what());
    }
    if (result.timed_out) throw DcDeterminismError("the toolchain probe timed out after 600 seconds");
    std::string out = scrub_utf8(result.out);
    std::string line;
    for (const std::string& candidate : py_splitlines(out)) {
        if (!py_strip(candidate).empty()) line = py_strip(candidate);
    }
    Json parsed = Json::parse(line, nullptr, false);
    if (result.exit_code != 0 || parsed.is_discarded() || !parsed.is_object()) {
        throw DcDeterminismError("the toolchain probe failed (exit code " + std::to_string(result.exit_code) + "):\n" +
                         last_lines(out + scrub_utf8(result.err), 40));
    }
    return parsed;
}

// `run`: one command inside the toolchain environment; `what` names it in the errors.
void script_run(const fs::path& root, const std::string& what, const std::string& kind, const std::string& compiler,
                const fs::path& cwd, const std::vector<std::string>& argv) {
    TempDir scratch("dc_determinism-build-");
    fs::path command_file = scratch.path() / "command.txt";
    std::string content;
    for (const std::string& part : argv) content += part + "\n";
    {
        std::ofstream out(command_file, std::ios::binary);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) throw DcDeterminismError("cannot write " + py_str(command_file));
    }
    std::vector<std::string> arguments = {"-Action", "run", "-Kind", kind, "-Compiler", compiler, "-CommandFile",
                                          path_to_utf8(command_file)};
    if (!cwd.empty()) {
        arguments.push_back("-Cwd");
        arguments.push_back(path_to_utf8(cwd));
    }
    StartOptions options;
    options.argv = script_command(root, arguments);
    RunResult result;
    try {
        result = run_capture(options, std::string(), 0.0);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("cannot run the build script: ") + error.what());
    }
    if (result.exit_code == 0) return;
    std::string err = scrub_utf8(result.err);
    static const std::string kError = "DC-DETERMINISM-BUILD-ERROR: ";
    static const std::string kStart = "DC-DETERMINISM-BUILD-START-FAILED: ";
    for (const std::string& line : py_splitlines(err)) {
        if (line.compare(0, kError.size(), kError) == 0) throw DcDeterminismError(line.substr(kError.size()));
        if (line.compare(0, kStart.size(), kStart) == 0) {
            throw DcDeterminismError("cannot run " + what + ": " + line.substr(kStart.size()));
        }
    }
    throw DcDeterminismError(what + " failed (exit code " + std::to_string(result.exit_code) + "):\n" +
                     last_lines(scrub_utf8(result.out) + err, 40));
}

}  // namespace

BuildHooks& build_hooks() {
    static BuildHooks hooks;
    return hooks;
}

namespace {

BuildToolchain probe_toolchain(const fs::path& root, const std::string& kind, const std::string& compiler) {
    Json answer = build_hooks().probe ? build_hooks().probe(kind, compiler) : script_probe(root, kind, compiler);
    if (json_get(answer, "ok") != Json(true)) {
        Json message = json_get(answer, "error");
        throw DcDeterminismError(message.is_string() ? message.get<std::string>() : std::string("the toolchain probe failed"));
    }
    auto text = [&](const char* key) {
        Json value = json_get(answer, key);
        return value.is_string() ? value.get<std::string>() : std::string();
    };
    BuildToolchain toolchain;
    toolchain.id = text("id");
    toolchain.cxx = text("cxx");
    toolchain.cc = text("cc");
    toolchain.version = text("version");
    toolchain.target = text("target");
    toolchain.bin_dir = text("bin_dir");
    toolchain.generator = text("generator");
    Json generator_args = json_get(answer, "generator_args");
    if (generator_args.is_array()) {
        for (const Json& item : generator_args) {
            if (item.is_string()) toolchain.generator_args.push_back(item.get<std::string>());
        }
    }
    toolchain.cmake = text("cmake");
    toolchain.cmake_version = text("cmake_version");
    toolchain.node = text("node");
    toolchain.node_version = text("node_version");
    Json facts = json_get(answer, "facts");
    if (facts.is_object()) toolchain.facts = facts;
    return toolchain;
}

void run_tool(const fs::path& root, const std::string& what, const std::string& kind, const std::string& compiler,
              const fs::path& cwd, const std::vector<std::string>& argv) {
    if (build_hooks().run) {
        build_hooks().run(what, kind, compiler, cwd, argv);
    } else {
        script_run(root, what, kind, compiler, cwd, argv);
    }
}

// --- identity ---------------------------------------------------------------------------------------

Json revision_entry(const fs::path& root, const std::string& name) {
    Json entry = json_get(read_json(rel(root, kManifestRel)), name.c_str());
    if (!entry.is_object()) {
        throw DcDeterminismError(std::string(kManifestRel) + " has no entry for " + name);
    }
    return entry;
}

Json tree_identity(const Json& entry, const std::string& where) {
    Json out = Json::object();
    out["revision"] = required_member(entry, "revision", where);
    out["git_tree"] = required_member(entry, "git_tree", where);
    out["tree_sha256_lf"] = required_member(entry, "tree_sha256_lf", where);
    return out;
}

std::string tree_hash_of(const Json& entry, const std::string& where) {
    Json value = required_member(entry, "tree_sha256_lf", where);
    return value.is_string() ? value.get<std::string>() : py_repr(value);
}

Json dependency_identity(const fs::path& root, const std::vector<std::string>& names) {
    Json manifest = read_json(rel(root, kManifestRel));
    Json dependencies = json_get(manifest, "dependencies", Json::object());
    Json result = Json::object();
    for (const std::string& name : names) {
        Json entry = json_get(dependencies, name.c_str());
        if (!entry.is_object()) {
            throw DcDeterminismError(std::string(kManifestRel) + " has no entry for " + name);
        }
        std::string where = std::string(kManifestRel) + " entry " + name;
        verified_tree(rel(root, std::string(kDepsRel) + "/" + name), tree_hash_of(entry, where), "dependency " + name);
        Json item = Json::object();
        item["pin"] = required_member(entry, "pin", where);
        item["git_tree"] = required_member(entry, "git_tree", where);
        item["tree_sha256_lf"] = required_member(entry, "tree_sha256_lf", where);
        result[name] = item;
    }
    return result;
}

// `_normalized`: machine-specific path prefixes replaced, so that identities are stable across checkouts.
Json normalized(const Json& value, const std::vector<std::pair<std::string, std::string>>& replacements) {
    if (value.is_string()) {
        std::string text = value.get<std::string>();
        for (const auto& item : replacements) {
            if (item.first.empty()) continue;
            std::string out;
            std::size_t position = 0;
            for (;;) {
                std::size_t found = text.find(item.first, position);
                if (found == std::string::npos) break;
                out.append(text, position, found - position);
                out += item.second;
                position = found + item.first.size();
            }
            out.append(text, position, std::string::npos);
            text = out;
        }
        return text;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(normalized(item, replacements));
        return out;
    }
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) out[it.key()] = normalized(it.value(), replacements);
        return out;
    }
    return value;
}

Json to_json(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& value : values) out.push_back(value);
    return out;
}

// --- receipts and cache -----------------------------------------------------------------------------

// `_cached_receipt`: the stored receipt when its identity equals `identity` and every recorded output is intact.
bool cached_receipt(const fs::path& directory, const Json& identity, Json& receipt) {
    fs::path path = directory / kBuildReceiptName;
    if (!is_file(path)) return false;
    Json parsed;
    try {
        parsed = parse_json_text(read_file(path), py_str(path));
    } catch (const DcDeterminismError&) {
        return false;
    }
    if (!parsed.is_object() || json_get(parsed, "identity") != identity) return false;
    Json output = json_get(parsed, "output");
    if (!output.is_object()) return false;
    std::vector<std::pair<fs::path, Json>> files;
    Json file = json_get(output, "file");
    Json digest = json_get(output, "sha256");
    if (!file.is_string() || !output.contains("sha256")) return false;
    files.emplace_back(directory / path_from_utf8(file.get<std::string>()), digest);
    Json sidecars = output.contains("sidecars") ? output["sidecars"] : Json::object();
    if (!sidecars.is_object()) return false;
    for (auto it = sidecars.begin(); it != sidecars.end(); ++it) {
        files.emplace_back(directory / path_from_utf8(it.key()), it.value());
    }
    for (const auto& item : files) {
        try {
            if (!is_file(item.first) || Json(sha256_file(item.first)) != item.second) return false;
        } catch (const DcDeterminismError&) {
            return false;
        }
    }
    receipt = parsed;
    return true;
}

void write_receipt(const fs::path& directory, const Json& receipt) {
    fs::path path = directory / kBuildReceiptName;
    std::string text = json_dumps(receipt, DumpOptions::pretty(2)) + "\n";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out) throw DcDeterminismError("cannot write " + py_str(path));
}

void remove_tree(const fs::path& directory) {
    std::error_code ec;
    fs::remove_all(directory, ec);
    if (ec) throw DcDeterminismError("cannot remove " + py_str(directory) + ": " + ec.message());
}

// `_reset_directory`.
void reset_directory(const fs::path& directory) {
    std::error_code ec;
    if (fs::exists(directory, ec)) remove_tree(directory);
    fs::create_directories(directory, ec);
    if (ec) throw DcDeterminismError("cannot create " + py_str(directory) + ": " + ec.message());
}

void copy_file_preserving(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) throw DcDeterminismError("cannot copy " + py_str(from) + " to " + py_str(to) + ": " + ec.message());
    fs::file_time_type stamp = fs::last_write_time(from, ec);
    if (!ec) fs::last_write_time(to, stamp, ec);
}

void copy_tree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) throw DcDeterminismError("cannot create " + py_str(to) + ": " + ec.message());
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path target = to / it->path().filename();
        std::error_code type_ec;
        if (fs::is_directory(it->path(), type_ec)) {
            copy_tree(it->path(), target);
        } else {
            copy_file_preserving(it->path(), target);
        }
    }
    if (ec) throw DcDeterminismError("cannot read " + py_str(from) + ": " + ec.message());
}

std::string utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", &utc);
    return buffer;
}

const char* system_name() { return is_windows() ? "Windows" : "Linux"; }
std::string machine_name() {
#ifdef _WIN32
    return "AMD64";
#else
    struct utsname name;
    if (uname(&name) != 0) return "x86_64";
    return name.machine;
#endif
}

Json host_description() {
    Json host = Json::object();
    host["system"] = system_name();
    host["machine"] = machine_name();
    host["platform"] = host_platform();
    host["python"] = nullptr;  // this tool has no Python
    return host;
}

// `variant`: the id of a `*-nofma` compiler (sim/compilers.hpp), empty for the others. Only a variant adds
// fields to the receipt (`compiler.variant`, `compiler.flags_extra`); the receipt of every other compiler keeps
// the format of earlier receipts.
Json make_receipt(const std::string& kind, const Json& identity, const BuildToolchain& toolchain, const Json& flags,
                  const Json& output, const std::string& variant = std::string()) {
    Json compiler = Json::object();
    compiler["id"] = toolchain.id;
    compiler["executable"] = posix(toolchain.cxx);
    compiler["version"] = toolchain.version;
    compiler["target"] = toolchain.target;
    if (!variant.empty()) {
        compiler["variant"] = variant;
        compiler["flags_extra"] = to_json(extra_compiler_flags(variant));
    }
    Json receipt = Json::object();
    receipt["schema"] = kBuildReceiptSchema;
    receipt["kind"] = kind;
    receipt["compiler"] = compiler;
    receipt["flags"] = flags;
    receipt["sources"] = identity["sources"];
    receipt["output"] = output;
    receipt["host"] = host_description();
    receipt["built_at"] = utc_now();
    receipt["identity"] = identity;
    return receipt;
}

std::string jobs() {
    std::optional<std::string> value = env_get("DC_DETERMINISM_BUILD_JOBS");
    if (value) {
        std::int64_t parsed = 0;
        if (py_parse_int(py_strip(*value), parsed)) return std::to_string(std::max<std::int64_t>(1, parsed));
    }
    return std::to_string(std::max(1, std::min(cpu_count(), 8)));
}

fs::path effective_root(const fs::path& root) { return root.empty() ? resolve(repo_root()) : resolve(root); }

// --- loader -----------------------------------------------------------------------------------------

std::string joined_flags(const std::vector<std::string>& flags) {
    std::string out;
    for (const std::string& flag : flags) out += (out.empty() ? "" : " ") + flag;
    return out;
}

std::vector<std::string> loader_configure_args(const fs::path& root, const BuildToolchain& toolchain,
                                               const std::string& generator,
                                               const std::vector<std::string>& generator_args,
                                               const std::vector<std::pair<std::string, fs::path>>& source_dirs,
                                               const fs::path& build_dir,
                                               const std::vector<std::string>& extra_flags) {
    std::vector<std::string> args = {"-S", posix(rel(root, kSimulatorRel)), "-B", posix(build_dir), "-G", generator};
    args.insert(args.end(), generator_args.begin(), generator_args.end());
    if (toolchain.id != "msvc") {
        args.push_back("-DCMAKE_BUILD_TYPE=Release");
        args.push_back("-DCMAKE_CXX_COMPILER=" + posix(toolchain.cxx));
        args.push_back("-DCMAKE_C_COMPILER=" + posix(toolchain.cc));
    }
    args.push_back("-DCMAKE_CXX_STANDARD=17");
    if (is_windows()) {
        args.push_back("-DCMAKE_CXX_STANDARD_REQUIRED=ON");
        // The `_INIT` variables keep the platform defaults (/EHsc, /DWIN32, ...) of the compiler flags.
        ReproducibleFlags reproducible = reproducible_flags(toolchain.id, root);
        std::string compile = joined_flags(reproducible.compile);
        std::string link = joined_flags(reproducible.link);
        if (!compile.empty()) {
            args.push_back("-DCMAKE_CXX_FLAGS_INIT=" + compile);
            args.push_back("-DCMAKE_C_FLAGS_INIT=" + compile);
        }
        if (!link.empty()) {
            for (const char* kind : {"SHARED", "MODULE", "EXE"}) {
                args.push_back(std::string("-DCMAKE_") + kind + "_LINKER_FLAGS_INIT=" + link);
            }
        }
    } else {
        args.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
        std::string cxx_flags = "-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations";
        std::string c_flags;
        for (const std::string& flag : extra_flags) {
            cxx_flags += " " + flag;
            c_flags += (c_flags.empty() ? "" : " ") + flag;
        }
        args.push_back(cxx_flags);
        if (!c_flags.empty()) args.push_back("-DCMAKE_C_FLAGS=" + c_flags);
    }
    for (const char* option :
         {"-DBOX2D_BUILD_TESTBED=OFF", "-DBOX2D_BUILD_UNIT_TESTS=OFF", "-DDIGITALCURLING_BUILD_PLAYERS=OFF",
          "-DDIGITALCURLING_BUILD_SIMULATORS=ON", "-DDIGITALCURLING_BUILD_SIMULATOR_FCV1=ON",
          "-DDIGITALCURLING_BUILD_SIMULATOR_FCV1PLUS=ON", "-DDIGITALCURLING_BUILD_PLUGIN_LOADER=ON",
          "-DDIGITALCURLING_PLUGIN_LOADER_SHARED=ON", "-DDIGITALCURLING_BUNDLE_PLUGINS=ON",
          "-DDIGITALCURLING_BUILD_TEST=OFF", "-DDIGITALCURLING_BUILD_DOCS=OFF"}) {
        args.push_back(option);
    }
    for (const auto& item : source_dirs) args.push_back("-DFETCHCONTENT_SOURCE_DIR_" + item.first + "=" + posix(item.second));
    args.push_back("-DFETCHCONTENT_FULLY_DISCONNECTED=ON");
    return args;
}

std::vector<fs::path> loader_output_candidates(const fs::path& build_dir, const std::string& compiler) {
    fs::path directory = build_dir / "src" / "plugin-loader";
    if (compiler == "msvc") return {directory / "Release" / "digitalcurling_plugin_loader.dll"};
    if (is_windows()) {
        return {directory / "libdigitalcurling_plugin_loader.dll", directory / "digitalcurling_plugin_loader.dll"};
    }
    return {directory / "libdigitalcurling_plugin_loader.so"};
}

void check_compiler(const std::string& compiler) {
    if (!is_native_compiler(compiler)) {
        throw DcDeterminismError("unknown compiler for a local build: " + compiler + " (choose from " +
                         local_compiler_text(false) + ")");
    }
}

}  // namespace

std::string host_platform() {
#if defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif !(defined(__x86_64__) || defined(_M_X64))
    throw DcDeterminismError("local builds run on Windows x86-64 hosts only (this host: unsupported)");
#elif defined(_WIN32)
    return "windows-amd64";
#elif defined(__linux__)
    return "linux-amd64";
#else
    throw DcDeterminismError("local builds run on Windows x86-64 hosts only (this host: unsupported)");
#endif
}

fs::path cache_base(const fs::path& root) {
    if (!build_hooks().cache.empty()) return build_hooks().cache;
    return root / "build" / host_platform();
}

std::string verified_tree(const fs::path& directory, const std::string& expected, const std::string& label) {
    if (!directory_exists(directory)) throw DcDeterminismError("vendored " + label + " is missing: " + py_str(directory));
    std::string actual = tree_sha256_lf(directory);
    if (actual != expected) {
        throw DcDeterminismError("vendored " + label + " does not match its recorded tree hash (expected " + expected +
                         ", found " + actual + "): " + py_str(directory));
    }
    return actual;
}

Json loader_sources(const fs::path& root) {
    Json entry = revision_entry(root, "digitalcurling");
    std::string where = std::string(kManifestRel) + " entry digitalcurling";
    verified_tree(rel(root, kSimulatorRel), tree_hash_of(entry, where), "simulator source");
    std::vector<std::string> names;
    for (const auto& item : kLoaderDependencies) names.push_back(item.second);
    Json sources = Json::object();
    sources["simulator"] = tree_identity(entry, where);
    sources["dependencies"] = dependency_identity(root, names);
    return sources;
}

Json player_sources(const fs::path& root) {
    Json entry = revision_entry(root, "digitalcurling");
    std::string where = std::string(kManifestRel) + " entry digitalcurling";
    verified_tree(rel(root, kPlayerLibRel), tree_hash_of(entry, where), "player library source");
    fs::path worker = rel(root, kPlayerWorkerRel);
    if (!is_file(worker)) throw DcDeterminismError("player worker source is missing: " + py_str(worker));
    Json sources = Json::object();
    sources["player_library"] = tree_identity(entry, where);
    sources["worker_cpp_sha256_lf"] = sha256_lf_file(worker);
    sources["dependencies"] = dependency_identity(
        root, std::vector<std::string>(std::begin(kPlayerDependencies), std::end(kPlayerDependencies)));
    return sources;
}

fs::path prepare_box2d(const fs::path& root, const fs::path& work) {
    fs::path destination = work / "box2d-patched";
    std::error_code ec;
    if (fs::exists(destination, ec)) remove_tree(destination);
    copy_tree(rel(root, std::string(kDepsRel) + "/box2d"), destination);
    fs::path cmake_file = destination / "CMakeLists.txt";
    std::string bytes = read_file(cmake_file);
    if (!is_valid_utf8(bytes)) throw DcDeterminismError("cannot decode " + py_str(cmake_file) + " as UTF-8");
    // `Path.read_text`: universal newlines.
    std::string text;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r') {
            if (i + 1 < bytes.size() && bytes[i + 1] == '\n') ++i;
            text.push_back('\n');
        } else {
            text.push_back(bytes[i]);
        }
    }
    static const std::regex pattern("cmake_minimum_required[ \\t]*\\(VERSION[ \\t]+3\\.[0-9]+\\)");
    std::ptrdiff_t count = std::distance(std::sregex_iterator(text.begin(), text.end(), pattern), std::sregex_iterator());
    if (count != 1) throw DcDeterminismError("the Box2D CMake compatibility patch did not match exactly once");
    std::string patched = std::regex_replace(text, pattern, "cmake_minimum_required(VERSION 3.15)");
    std::ofstream out(cmake_file, std::ios::binary | std::ios::trunc);
    out.write(patched.data(), static_cast<std::streamsize>(patched.size()));
    out.close();
    if (!out) throw DcDeterminismError("cannot write " + py_str(cmake_file));
    return destination;
}

ReproducibleFlags reproducible_flags(const std::string& compiler_id, const fs::path& root) {
    ReproducibleFlags flags;
    if (!is_windows()) return flags;
    const std::string compiler = base_compiler(compiler_id);
    if (compiler == "msvc") {
        flags.compile = {"/Brepro"};
        flags.link = {"/Brepro"};
    } else if (compiler == "gcc" || compiler == "clang") {
        flags.compile = {"-ffile-prefix-map=" + posix(root) + "=."};
        flags.link = {"-Wl,--no-insert-timestamp"};
    }
    return flags;
}

BuiltLoader build_loader(const std::string& compiler, const fs::path& root_given, bool reuse_cache) {
    fs::path root = effective_root(root_given);
    check_compiler(compiler);
    require_compiler_platform(compiler, host_platform());
    // A variant (gcc-nofma) is built by its base compiler (the build script knows only the base ids).
    const std::string base_id = base_compiler(compiler);
    const std::vector<std::string> extra_flags = extra_compiler_flags(compiler);
    BuildToolchain toolchain = probe_toolchain(root, "loader", base_id);
    Json sources = loader_sources(root);
    fs::path base = cache_base(root) / path_from_utf8(compiler) / "loader";
    fs::path build_dir = base / "cmake";
    fs::path work = base / "inputs";
    fs::path output_dir = base / "bin";
    std::vector<std::pair<std::string, fs::path>> source_dirs;
    for (const auto& item : kLoaderDependencies) {
        fs::path directory = std::string(item.first) == "BOX2D" ? work / "box2d-patched"
                                                                 : rel(root, std::string(kDepsRel) + "/" + item.second);
        source_dirs.emplace_back(item.first, directory);
    }
    std::vector<std::string> configure =
        loader_configure_args(root, toolchain, toolchain.generator, toolchain.generator_args, source_dirs, build_dir,
                              extra_flags);
    // `build[1:-1]` of the full command: without the executable and without the job count.
    std::vector<std::string> build_flags = {"--build", posix(build_dir), "--config", "Release", "--target",
                                            "digitalcurling_plugin_loader", "--parallel"};
    std::vector<std::pair<std::string, std::string>> replacements = {{posix(base), "<build>"}, {posix(root), "<repo>"}};
    std::vector<std::string> all_flags = configure;
    all_flags.insert(all_flags.end(), build_flags.begin(), build_flags.end());

    Json compiler_identity = Json::object();
    compiler_identity["id"] = compiler;
    compiler_identity["version"] = toolchain.version;
    compiler_identity["target"] = toolchain.target;
    Json identity = Json::object();
    identity["recipe"] = kRecipe;
    identity["kind"] = "loader";
    identity["sources"] = sources;
    identity["compiler"] = compiler_identity;
    identity["cmake"] = toolchain.cmake_version;
    identity["generator"] = toolchain.generator;
    identity["flags"] = normalized(to_json(all_flags), replacements);

    Json cached;
    if (reuse_cache && cached_receipt(base, identity, cached)) {
        // `str(Path)` has the platform's separators; a loader path with `/` cannot be loaded on Windows.
        fs::path library = base / path_from_utf8(cached["output"]["file"].get<std::string>());
        library.make_preferred();
        log("[build] reusing the cached " + compiler + " loader: " + py_str(library));
        return BuiltLoader{library, cached};
    }
    log("[build] building the " + compiler + " loader (" + toolchain.version + ") in " + py_str(base));
    std::error_code ec;
    fs::create_directories(base, ec);
    fs::remove(base / kBuildReceiptName, ec);
    for (const fs::path& stale : {build_dir, work, output_dir}) reset_directory(stale);
    prepare_box2d(root, work);

    std::vector<std::string> configure_command = {toolchain.cmake};
    configure_command.insert(configure_command.end(), configure.begin(), configure.end());
    run_tool(root, "cmake configure (" + compiler + " loader)", "loader", base_id, fs::path(), configure_command);
    std::vector<std::string> build_command = {toolchain.cmake};
    build_command.insert(build_command.end(), build_flags.begin(), build_flags.end());
    build_command.push_back(jobs());
    run_tool(root, "cmake build (" + compiler + " loader)", "loader", base_id, fs::path(), build_command);

    fs::path built;
    for (const fs::path& candidate : loader_output_candidates(build_dir, compiler)) {
        if (is_file(candidate)) {
            built = candidate;
            break;
        }
    }
    if (built.empty()) {
        throw DcDeterminismError("the loader library was not created below " + py_str(build_dir / "src" / "plugin-loader"));
    }
    std::string built_name = path_to_utf8(built.filename());
    copy_file_preserving(built, output_dir / built.filename());
    Json sidecars = Json::object();
    if (compiler != "msvc" && is_windows()) {
        for (const char* name : kMingwRuntimeDlls) {
            fs::path source = path_from_utf8(toolchain.bin_dir) / name;
            if (is_file(source)) {
                copy_file_preserving(source, output_dir / name);
                sidecars[std::string("bin/") + name] = sha256_file(output_dir / name);
            }
        }
    }
    Json output = Json::object();
    output["file"] = "bin/" + built_name;
    output["sha256"] = sha256_file(output_dir / built.filename());
    output["sidecars"] = sidecars;
    Json flags = Json::object();
    flags["generator"] = toolchain.generator;
    flags["cmake"] = toolchain.cmake_version;
    Json identity_flags = identity["flags"];
    Json configure_part = Json::array();
    Json build_part = Json::array();
    for (std::size_t i = 0; i < identity_flags.size(); ++i) {
        (i < configure.size() ? configure_part : build_part).push_back(identity_flags[i]);
    }
    flags["configure"] = configure_part;
    flags["build"] = build_part;
    Json receipt = make_receipt("loader", identity, toolchain, flags, output, is_variant_compiler(compiler) ? compiler : std::string());
    write_receipt(base, receipt);
    fs::path library = base / path_from_utf8(receipt["output"]["file"].get<std::string>());
    library.make_preferred();
    return BuiltLoader{library, receipt};
}

// --- players ----------------------------------------------------------------------------------------

std::vector<std::string> player_flags(const std::string& compiler_id, const std::string& player) {
    const std::string compiler = base_compiler(compiler_id);
    if (compiler == "msvc") {
        std::vector<std::string> flags = {"/nologo", "/std:c++17", "/EHsc", "/O2", "/DNDEBUG", "/wd4996", "/utf-8"};
        if (player == "nd2") flags.push_back("/DPLAYER_ND2");
        return flags;
    }
    std::vector<std::string> flags = {"-std=c++17", "-O3", "-DNDEBUG", "-Wno-deprecated-declarations"};
    if (compiler == "wasm") {
        for (const char* flag : {"-fexceptions", "-sNODERAWFS=1", "-sALLOW_MEMORY_GROWTH=1", "-sENVIRONMENT=node"}) {
            flags.push_back(flag);
        }
    }
    if (player == "nd2") flags.push_back("-DPLAYER_ND2");
    if (compiler == "gcc" || compiler == "clang") {
        if (is_windows()) {
            flags.push_back("-static");
        } else {
            flags.push_back("-static-libstdc++");
            flags.push_back("-static-libgcc");
        }
    }
    for (const std::string& flag : extra_compiler_flags(compiler_id)) flags.push_back(flag);
    return flags;
}

// `player_inputs`: (include directories, sources) of one player worker.
std::pair<std::vector<fs::path>, std::vector<fs::path>> player_inputs(const fs::path& root, const std::string& player) {
    fs::path library = rel(root, kPlayerLibRel);
    fs::path player_src = library / "src" / "player" / "src";
    std::vector<fs::path> includes = {library / "src" / "core" / "include",
                                      library / "src" / "plugin-api" / "include",
                                      player_src / path_from_utf8(player),
                                      player_src / "nd2",
                                      rel(root, std::string(kDepsRel) + "/nlohmann-json/include"),
                                      rel(root, std::string(kDepsRel) + "/fpm/include")};
    fs::path player_dir = player_src / path_from_utf8(player);
    std::vector<fs::path> sources = {rel(root, kPlayerWorkerRel),
                                     player_dir / path_from_utf8("player_" + player + ".cpp"),
                                     player_dir / path_from_utf8("player_" + player + "_factory.cpp"),
                                     player_dir / path_from_utf8("player_" + player + "_storage.cpp"),
                                     player_src / "nd2" / "xoshiro256plusplus.cpp"};
    return {includes, sources};
}

BuildToolchain probe_build_toolchain(const fs::path& root, const std::string& kind, const std::string& compiler) {
    return probe_toolchain(root, kind, compiler);
}

void run_build_tool(const fs::path& root, const std::string& what, const std::string& kind, const std::string& compiler,
                    const fs::path& cwd, const std::vector<std::string>& argv) {
    run_tool(root, what, kind, compiler, cwd, argv);
}

BuiltPlayer build_player(const std::string& player, const std::string& compiler, const fs::path& root_given) {
    if (!contains(kPlayers, player)) {
        throw DcDeterminismError("unknown player: " + player + " (choose from " + joined(kPlayers, 2, ", ") + ")");
    }
    if (compiler != "wasm" && !is_native_compiler(compiler)) {
        throw DcDeterminismError("unknown compiler for a local player build: " + compiler + " (choose from " +
                         local_compiler_text(true) + ")");
    }
    fs::path root = effective_root(root_given);
    if (compiler != "wasm") require_compiler_platform(compiler, host_platform());
    const std::string base_id = base_compiler(compiler);
    BuildToolchain toolchain = probe_toolchain(root, "player", base_id);
    Json sources = player_sources(root);
    fs::path base = cache_base(root) / path_from_utf8(compiler) / path_from_utf8("player-" + player);
    std::string exe_name = compiler == "wasm" ? "worker.js" : (is_windows() ? "worker.exe" : "worker");
    std::vector<std::string> flags = player_flags(compiler, player);
    std::pair<std::vector<fs::path>, std::vector<fs::path>> inputs = player_inputs(root, player);
    std::vector<std::pair<std::string, std::string>> replacements = {{posix(base), "<build>"}, {posix(root), "<repo>"}};

    Json compiler_identity = Json::object();
    compiler_identity["id"] = compiler;
    compiler_identity["version"] = toolchain.version;
    compiler_identity["target"] = toolchain.target;
    Json identity = Json::object();
    identity["recipe"] = kRecipe;
    identity["kind"] = "player";
    identity["player"] = player;
    identity["sources"] = sources;
    identity["compiler"] = compiler_identity;
    identity["flags"] = normalized(to_json(flags), replacements);
    if (compiler == "wasm") identity["node"] = toolchain.node_version;

    auto prefix = [&](const fs::path& path) {
        std::vector<std::string> command;
        if (compiler == "wasm") command.push_back(toolchain.node);
        command.push_back(py_str(path));
        return command;
    };

    Json cached;
    if (cached_receipt(base, identity, cached)) {
        fs::path worker = base / path_from_utf8(cached["output"]["file"].get<std::string>());
        log("[build] reusing the cached " + compiler + " " + player + " player: " + py_str(worker));
        return BuiltPlayer{prefix(worker), cached};
    }
    log("[build] building the " + compiler + " " + player + " player (" + toolchain.version + ") in " + py_str(base));
    reset_directory(base);
    fs::path output = base / path_from_utf8(exe_name);
    std::vector<std::string> command = {toolchain.cxx};
    command.insert(command.end(), flags.begin(), flags.end());
    if (compiler == "msvc") {
        for (const fs::path& include : inputs.first) command.push_back("/I" + py_str(include));
        for (const fs::path& source : inputs.second) command.push_back(py_str(source));
        command.push_back("/Fe:" + py_str(output));
        command.push_back("/Fo:" + py_str(base) + (is_windows() ? "\\" : "/"));
    } else {
        for (const fs::path& include : inputs.first) {
            command.push_back("-I");
            command.push_back(py_str(include));
        }
        for (const fs::path& source : inputs.second) command.push_back(py_str(source));
        command.push_back("-o");
        command.push_back(py_str(output));
    }
    run_tool(root, compiler + " build of the " + player + " player", "player", base_id, base, command);
    if (!is_file(output)) throw DcDeterminismError("the player worker was not created: " + py_str(output));
    Json sidecars = Json::object();
    if (compiler == "wasm") {
        fs::path wasm = base / "worker.wasm";
        if (!is_file(wasm)) throw DcDeterminismError("the wasm player module was not created: " + py_str(wasm));
        sidecars["worker.wasm"] = sha256_file(wasm);
    }
    Json result = Json::object();
    result["file"] = exe_name;
    result["sha256"] = sha256_file(output);
    result["sidecars"] = sidecars;
    Json receipt_flags = Json::object();
    receipt_flags["compile"] = identity["flags"];
    receipt_flags["player"] = player;
    Json receipt = make_receipt("player", identity, toolchain, receipt_flags, result,
                                is_variant_compiler(compiler) ? compiler : std::string());
    write_receipt(base, receipt);
    return BuiltPlayer{prefix(output), receipt};
}

}  // namespace dc_determinism
