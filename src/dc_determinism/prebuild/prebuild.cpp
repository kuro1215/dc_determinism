#include "prebuild/prebuild.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>

#ifndef _WIN32
#include <sys/utsname.h>
#include <unistd.h>

extern char** environ;
#endif

#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/scratch.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "common/tree_hash.hpp"
#include "json/writer.hpp"
#include "localbuild/local_build.hpp"
#include "player/pdet.hpp"
#include "prebuild/binary_sections.hpp"
#include "process/process.hpp"
#include "sim/wasm.hpp"

namespace dc_determinism {

const char* const kPrebuiltManifestSchema = "dc-determinism-prebuilt-v2";
const char* const kDefaultEmsdkImage = "emscripten/emsdk:6.0.9";

namespace {

const char* const kWorkerRel = "src/worker/worker.cpp";
const char* const kPlayerWorkerRel = "src/worker/player.cpp";
const char* const kPlayerLibraryRel = "src/vendor/digitalcurling";
const char* const kManifestRel = "prebuilt/manifest.json";
const char* const kWindowsWorkerRel = "prebuilt/worker/windows-amd64/dc_worker.exe";
const char* const kArm64Key = "linux-arm64";
const char* const kArm64WorkerDirRel = "prebuilt/worker/linux-arm64";
const char* const kArm64PlayerPrefix = "linux-arm64-";
// The host that rebuilds the linux-arm64 data this host removed (the hint of the log line).
const char* const kArm64Rebuild =
    "an aarch64 Linux host (scripts/prebuild.sh, then prebuilt/dc_determinism/linux-arm64/dc_determinism "
    "prebuild) or an Apple-silicon Mac (scripts/prebuild.sh, then dc_determinism.sh prebuild)";
const char* const kPreferredImage = "dc_determinism-cpu:local";
const char* const kPlayers[] = {"normal_dist", "nd2"};

// The WebAssembly dc_worker: its bundle, its sources and the container script that builds it.
const char* const kWasmBundleRel = "prebuilt/worker/wasm";
const char* const kWasmBuildInfoSchema = "dc-determinism-wasm-build-info-v1";
const char* const kWasmSmokeInputRel = "src/worker/wasm/smoke-input.bin";
const char* const kWasmBuildScriptRel = "src/worker/wasm/build-wasm-worker.sh";
const char* const kWasmScriptInContainer = "/repo/src/worker/wasm/build-wasm-worker.sh";
const char* const kSimulatorRel = "src/vendor/digitalcurling";
const char* const kDepsRel = "src/vendor";
const char* const kVendorManifestRel = "src/vendor/manifest.json";
const char* const kLoaderPlatforms[] = {"windows-amd64", "linux-amd64", "linux-arm64"};
const char* const kLoaderDependencies[] = {"fpm", "nlohmann-json", "box2d", "xxhash"};
const char* const kWasmEngines[] = {"fcv1", "fcv1plus"};
constexpr int kWasmSmokeShots = 3;

// The vendored dependencies of the players: manifest key -> directory below src/vendor.
const std::pair<const char*, const char*> kPlayerDependencies[] = {
    {"fpm", "fpm"},
    {"nlohmann_json", "nlohmann-json"},
};

std::string native(fs::path path) {
    path.make_preferred();
    return path_to_utf8(path);
}

std::string to_posix(std::string text) {
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

bool starts_with(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

std::vector<std::string> text_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    for (char c : text) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            line.clear();
        } else {
            line.push_back(c);
        }
    }
    if (!line.empty()) {
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string first_line(const std::string& text) {
    std::vector<std::string> lines = text_lines(text);
    return lines.empty() ? std::string() : py_strip(lines.front());
}

std::string tail(const std::string& text, std::size_t count) {
    std::vector<std::string> lines = text_lines(text);
    std::size_t start = lines.size() > count ? lines.size() - count : 0;
    std::string out;
    for (std::size_t i = start; i < lines.size(); ++i) out += (i == start ? "" : "\n") + lines[i];
    return out;
}

Json json_array_of(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const std::string& value : values) out.push_back(value);
    return out;
}

// A member of nested objects as text ("" when it is missing or not a string).
std::string member_text(const Json& value, std::initializer_list<const char*> path) {
    Json current = value;
    for (const char* key : path) current = json_get(current, key);
    return current.is_string() ? current.get<std::string>() : std::string();
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw DcDeterminismError("cannot write " + native(path));
}

// The scripts of the containers always have LF line endings.
void write_script(const fs::path& path, const std::string& script) { write_bytes(path, normalize_lf(script)); }

// Replaces `path` (a new file next to it is renamed over it).
void write_replacing(const fs::path& path, const std::string& bytes) {
    fs::path temporary = path;
    temporary += ".tmp";
    write_bytes(temporary, bytes);
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw DcDeterminismError("cannot replace " + native(path) + ": " + ec.message());
    }
}

// Replaces `to` by a copy of `from`. The copy is made next to `to` and renamed over it, so that a file that cannot
// be written in place (for example one owned by root on a mounted Windows drive) is still replaced.
void copy_replacing(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    fs::path temporary = to;
    temporary += ".tmp";
    fs::copy_file(from, temporary, fs::copy_options::overwrite_existing, ec);
    if (ec) throw DcDeterminismError("cannot copy " + native(from) + " to " + native(to) + ": " + ec.message());
    fs::rename(temporary, to, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw DcDeterminismError("cannot replace " + native(to) + ": " + ec.message());
    }
}

void make_executable(const fs::path& path) {
    if (is_windows()) return;
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                    fs::perm_options::add, ec);
}

// A directory every user of a container can write to (the Docker bind mounts of a Linux host).
void make_world_writable(const fs::path& path) {
    if (is_windows()) return;
    std::error_code ec;
    fs::permissions(path, fs::perms::all, fs::perm_options::replace, ec);
}

fs::path repo_root_of(const PrebuildOptions& options) {
    return resolve(options.root.empty() ? repo_root() : options.root);
}

// --- processes -----------------------------------------------------------------------------

std::string path_without_msys() {
    std::optional<std::string> path = env_get("PATH");
    if (!path) return std::string();
    const char separator = is_windows() ? ';' : ':';
    std::string out;
    std::string item;
    auto flush = [&] {
        std::string lower = item;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!item.empty() && lower.find("msys64") == std::string::npos) {
            out += (out.empty() ? "" : std::string(1, separator)) + item;
        }
        item.clear();
    };
    for (char c : *path) {
        if (c == separator) {
            flush();
        } else {
            item.push_back(c);
        }
    }
    flush();
    return out;
}

ToolOutput run_streamed(const std::vector<std::string>& argv, bool echo, bool without_msys) {
    StartOptions options;
    options.argv = argv;
    options.pipe_stdout = true;
    options.error_stream = ErrorStream::Stdout;
    if (without_msys) options.environment.emplace_back("PATH", path_without_msys());
    std::unique_ptr<Process> process = Process::start(options);
    ToolOutput result;
    std::string line;
    while (process->read_line(line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result.output += line + "\n";
        if (echo) log(line);
    }
    result.exit_code = process->wait();
    return result;
}

ToolOutput run_docker(const std::vector<std::string>& args, bool echo) {
    if (prebuild_hooks().docker) return prebuild_hooks().docker(args, echo);
    std::vector<std::string> argv = {"docker"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_streamed(argv, echo, false);
}

ToolOutput run_program(const std::vector<std::string>& argv, bool without_msys) {
    if (prebuild_hooks().program) return prebuild_hooks().program(argv, without_msys);
    return run_streamed(argv, false, without_msys);
}

ToolOutput checked_docker(const std::string& what, const std::vector<std::string>& args, bool echo) {
    ToolOutput result = run_docker(args, echo);
    if (result.exit_code != 0) {
        throw DcDeterminismError(what + " failed (exit code " + std::to_string(result.exit_code) + "):\n" +
                         tail(result.output, 40));
    }
    return result;
}

// A built program that must succeed; its output (stdout and stderr) is returned.
std::string checked_program(const std::string& what, const std::vector<std::string>& argv, bool without_msys) {
    ToolOutput result = run_program(argv, without_msys);
    if (result.exit_code != 0) {
        throw DcDeterminismError(what + " failed (exit code " + std::to_string(result.exit_code) + "):\n" +
                         tail(result.output, 40));
    }
    return result.output;
}

// --- the executors ---------------------------------------------------------------------------------------------
//
// The Linux build scripts run through the Docker CLI (the Docker executor), or, inside a container started with
// `--image-id`, directly (the in-container executor, see run_in_container). The state below is set for the duration of
// one run by ExecutorScope.

struct Executor {
    bool container = false;
    std::string image;     // container: the image the container runs
    std::string image_id;
    std::string user;      // container: the user of the builds (UID:GID)
};

Executor& executor() {
    static Executor state;
    return state;
}

bool in_container_now() {
    return prebuild_hooks().in_container ? prebuild_hooks().in_container() : environment::in_container();
}

bool windows_host_now() { return prebuild_hooks().windows_host ? prebuild_hooks().windows_host() : is_windows(); }

bool program_available(const std::string& name) {
    if (prebuild_hooks().has_program) return prebuild_hooks().has_program(name);
    return find_on_path(name).has_value();
}

class ExecutorScope {
public:
    explicit ExecutorScope(const PrebuildOptions& options) {
        bool container = in_container_now();
        if (!container && (!options.image_id.empty() || !options.build_user.empty())) {
            throw DcDeterminismError(
                "--image-id and --build-user are accepted only inside a container (they select the in-container "
                "executor that `dc_determinism.sh prebuild` uses on a Mac)");
        }
        Executor next;
        if (container) {
            if (options.wasm_worker_only) {
                throw DcDeterminismError("--wasm-worker-only needs the Docker CLI and cannot run inside a container");
            }
            if (options.emsdk_image_explicit) {
                throw DcDeterminismError("--emsdk-image needs the Docker CLI and cannot be used inside a container (wasm is not built there)");
            }
            if (options.image_id.empty()) {
                throw DcDeterminismError(
                    "`prebuild` needs the Docker CLI and cannot run inside a container without --image-id: run the "
                    "native binary on the host (dc_determinism-native.ps1 prebuild ... on Windows, "
                    "prebuilt/dc_determinism/linux-<arch>/dc_determinism on a Linux host, dc_determinism.sh prebuild on a Mac)");
            }
            if (options.image.empty()) {
                throw DcDeterminismError("--image-id needs --image (the name of the image this container runs)");
            }
            if (!options.build_user.empty()) {
                std::size_t colon = options.build_user.find(':');
                auto digits = [](const std::string& text) {
                    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                };
                if (colon == std::string::npos || !digits(options.build_user.substr(0, colon)) ||
                    !digits(options.build_user.substr(colon + 1))) {
                    throw DcDeterminismError("--build-user must be UID:GID (numbers), not " + options.build_user);
                }
                if (!program_available("setpriv")) {
                    throw DcDeterminismError(
                        "setpriv was not found in this container (util-linux): the in-container builds run as the "
                        "build user " + options.build_user + " through it");
                }
            }
            next.container = true;
            next.image = options.image;
            next.image_id = options.image_id;
            next.user = options.build_user;
        } else if (!prebuild_hooks().docker && !find_on_path("docker")) {
            throw DcDeterminismError("`prebuild` needs the Docker CLI (docker was not found on PATH)");
        }
        previous_ = executor();
        executor() = next;
    }
    ~ExecutorScope() { executor() = previous_; }
    ExecutorScope(const ExecutorScope&) = delete;
    ExecutorScope& operator=(const ExecutorScope&) = delete;

private:
    Executor previous_;
};

std::string docker_user() {
    if (executor().container) return executor().user;
    if (prebuild_hooks().docker_user) return prebuild_hooks().docker_user();
#ifdef _WIN32
    return std::string();
#else
    return std::to_string(getuid()) + ":" + std::to_string(getgid());
#endif
}

std::string select_image(const std::string& given) {
    if (executor().container) return executor().image;
    if (!given.empty()) return given;
    if (run_docker({"image", "inspect", kPreferredImage}, false).exit_code != 0) {
        throw DcDeterminismError(std::string("Docker image ") + kPreferredImage +
                         " not found; build it with `dc_determinism.sh image build` or `.\\dc_determinism.ps1 image build` (or pass --image)");
    }
    return kPreferredImage;
}

std::string image_id_of(const std::string& image) {
    if (executor().container && image == executor().image) return executor().image_id;
    ToolOutput result =
        checked_docker("docker image inspect " + image, {"image", "inspect", "--format", "{{.Id}}", image}, false);
    std::string id = first_line(result.output);
    if (id.empty()) throw DcDeterminismError("docker image inspect " + image + " printed no image id");
    return id;
}

// The machine of a Linux host (`uname -m`); empty where it is not asked (a Windows host without the hook).
std::string host_machine() {
    if (prebuild_hooks().host_machine) return prebuild_hooks().host_machine();
#ifdef _WIN32
    return std::string();
#else
    struct utsname name;
    if (uname(&name) != 0) return std::string();
    return name.machine;
#endif
}

// The linux platform a host builds: the one of its architecture (nothing is emulated).
struct LinuxTarget {
    std::string arch;      // amd64 or arm64
    std::string platform;  // linux-amd64 or linux-arm64

    bool arm64() const { return arch == "arm64"; }
    // The compiler ids of the loaders and players: gcc and clang, on arm64 also the two -ffp-contract=off variants.
    std::vector<std::string> compilers() const {
        if (arm64()) return {"gcc", "clang", "gcc-nofma", "clang-nofma"};
        return {"gcc", "clang"};
    }
    std::string worker_rel() const { return "prebuilt/worker/" + platform + "/dc_worker"; }
};

LinuxTarget linux_target_of_arch(const std::string& arch) {
    LinuxTarget target;
    target.arch = arch;
    target.platform = "linux-" + arch;
    return target;
}

// The Linux target of a host machine (`uname -m`); DcDeterminismError for any other machine.
LinuxTarget linux_target_of_machine(const std::string& machine) {
    if (machine == "x86_64" || machine == "amd64" || machine == "AMD64") return linux_target_of_arch("amd64");
    if (machine == "aarch64" || machine == "arm64") return linux_target_of_arch("arm64");
    throw DcDeterminismError("`dc_determinism prebuild` on a Linux host supports x86_64 and aarch64 machines (this host is " +
                             (machine.empty() ? std::string("unknown") : machine) + ")");
}

// The Docker image must have the architecture of the host: nothing is emulated. (Inside a container the image is the
// one the container runs.)
void require_image_architecture(const std::string& image, const LinuxTarget& target) {
    if (executor().container) return;
    ToolOutput result = checked_docker("docker image inspect " + image,
                                       {"image", "inspect", "--format", "{{.Architecture}}", image}, false);
    std::string architecture = first_line(result.output);
    if (architecture != target.arch) {
        throw DcDeterminismError("the Docker image " + image + " is " +
                                 (architecture.empty() ? std::string("of an unknown architecture") : architecture) +
                                 ", but `dc_determinism prebuild` on this host builds " + target.platform + " and needs an " +
                                 target.arch + " image (nothing is emulated); build one on this host with `dc_determinism.sh image build` "
                                 "or `.\\dc_determinism.ps1 image build`");
    }
}

struct Mount {
    fs::path host;
    std::string container;  // "/src:ro"
};

std::vector<std::string> docker_run_args(const std::string& image, const std::vector<Mount>& mounts,
                                         const std::vector<std::pair<std::string, std::string>>& environment,
                                         const fs::path& script, bool as_user) {
    std::vector<std::string> args = {"run", "--rm", "--entrypoint", "bash"};
    std::string user = as_user ? docker_user() : std::string();
    if (!user.empty()) {
        args.push_back("-u");
        args.push_back(user);
    }
    for (const auto& item : environment) {
        args.push_back("-e");
        args.push_back(item.first + "=" + item.second);
    }
    for (const Mount& mount : mounts) {
        args.push_back("-v");
        args.push_back(native(mount.host) + ":" + mount.container);
    }
    args.push_back("-v");
    args.push_back(native(script) + ":/build.sh:ro");
    args.push_back(image);
    args.push_back("/build.sh");
    return args;
}

bool has_line(const std::vector<std::string>& lines, const std::string& wanted) {
    return std::find(lines.begin(), lines.end(), wanted) != lines.end();
}

// --- the in-container executor -----------------------------------------------------------------------------------
//
// The same script, environment and mounts as the Docker executor, without Docker: every mount source is copied to its
// container path, the script runs under `env -i` as the build user, and the writable mounts are copied back. The
// builds therefore see the same paths in both executors and produce the same bytes.

const char* const kContainerMarkerDir = ".dc_determinism-prebuild-copies";

// What a container has of the `docker run` that is not the image's own: the variables of the wrapper (the command that
// started this container) and of the shell. Everything else of the container environment is the image's ENV.
const char* const kWrapperEnvironment[] = {"HOME",     "HOSTNAME",           "TERM",
                                           "PWD",      "OLDPWD",             "SHLVL",
                                           "_",        "DC_DETERMINISM_REPO", "DC_DETERMINISM_IMAGE",
                                           "DC_DETERMINISM_PREBUILD_SCRATCH", "PYTHONDONTWRITEBYTECODE"};
const char* const kDefaultContainerPath = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";

std::vector<std::pair<std::string, std::string>> container_environment() {
    if (prebuild_hooks().container_environment) return prebuild_hooks().container_environment();
    std::vector<std::pair<std::string, std::string>> items;
#ifndef _WIN32
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
        std::string text = *entry;
        std::size_t equals = text.find('=');
        if (equals == std::string::npos || equals == 0) continue;
        items.emplace_back(text.substr(0, equals), text.substr(equals + 1));
    }
#endif
    return items;
}

std::string container_hostname() {
    if (prebuild_hooks().container_hostname) return prebuild_hooks().container_hostname();
#ifndef _WIN32
    char buffer[256] = {0};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0) return buffer;
#endif
    return std::string();
}

fs::path container_base() {
    return prebuild_hooks().container_root.empty() ? fs::path("/") : prebuild_hooks().container_root;
}

std::string marker_name(const std::string& container_path) {
    std::string name = container_path.substr(container_path.empty() || container_path[0] != '/' ? 0 : 1);
    std::replace(name.begin(), name.end(), '/', '_');
    return name;
}

// A container path with its mount mode removed: "/src:ro" -> "/src".
std::string mount_path(const std::string& container) {
    std::size_t colon = container.find(':');
    return colon == std::string::npos ? container : container.substr(0, colon);
}

bool mount_read_only(const std::string& container) { return container.size() > 3 && container.compare(container.size() - 3, 3, ":ro") == 0; }

// Everything below `root` writable for every user (the copy of a writable mount: the build user is not the owner).
void make_tree_writable(const fs::path& root) {
    std::error_code ec;
    fs::permissions(root, fs::perms::all, fs::perm_options::replace, ec);
    if (!fs::is_directory(root, ec)) return;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry_ec;
        if (it->is_directory(entry_ec)) {
            fs::permissions(it->path(), fs::perms::all, fs::perm_options::replace, entry_ec);
        } else {
            fs::permissions(it->path(), fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                            fs::perm_options::add, entry_ec);
        }
    }
}

// The command line of the script: `env -i` with PATH, the image's ENV and exactly the variables the Docker executor
// passes with -e, HOME=/ and HOSTNAME as `docker run -u` sets them, as the build user through setpriv.
std::vector<std::string> in_container_command(const std::vector<std::pair<std::string, std::string>>& environment) {
    std::vector<std::string> argv = {"env", "-i"};
    std::vector<std::pair<std::string, std::string>> container = container_environment();
    std::string path = kDefaultContainerPath;
    for (const auto& item : container) {
        if (item.first == "PATH") path = item.second;
    }
    argv.push_back("PATH=" + path);
    for (const auto& item : container) {
        if (item.first == "PATH") continue;
        bool wrapper = false;
        for (const char* name : kWrapperEnvironment) wrapper = wrapper || item.first == name;
        if (!wrapper) argv.push_back(item.first + "=" + item.second);
    }
    for (const auto& item : environment) argv.push_back(item.first + "=" + item.second);
    argv.push_back("HOME=/");
    argv.push_back("HOSTNAME=" + container_hostname());
    const std::string& user = executor().user;
    if (!user.empty()) {
        std::size_t colon = user.find(':');
        argv.push_back("setpriv");
        argv.push_back("--reuid=" + user.substr(0, colon));
        argv.push_back("--regid=" + user.substr(colon + 1));
        argv.push_back("--clear-groups");
    }
    argv.push_back("bash");
    argv.push_back("/build.sh");
    return argv;
}

ToolOutput run_in_container(const std::string& what, const std::vector<Mount>& mounts,
                            const std::vector<std::pair<std::string, std::string>>& environment, const fs::path& script,
                            bool echo) {
    struct Copy {
        fs::path source;
        std::string path;       // the container path
        fs::path destination;   // where it is on disk
        bool read_only = true;
        bool directory = false;
        bool verified = false;  // /src and /deps/*: compared by tree hash after the script
        std::string expected_hash;
    };
    std::vector<Copy> copies;
    auto add = [&](const fs::path& source, const std::string& container) {
        Copy copy;
        copy.source = source;
        copy.path = mount_path(container);
        copy.destination = container_base() / path_from_utf8(copy.path.substr(1));
        copy.read_only = mount_read_only(container);
        std::error_code ec;
        copy.directory = fs::is_directory(source, ec);
        copy.verified = copy.read_only && copy.directory && (copy.path == "/src" || starts_with(copy.path, "/deps/"));
        copies.push_back(std::move(copy));
    };
    for (const Mount& mount : mounts) add(mount.host, mount.container);
    add(script, "/build.sh:ro");

    // Nothing is changed before every path was checked: a path that exists and was not made by a previous in-container
    // run (it has no marker) belongs to somebody else.
    fs::path markers = container_base() / kContainerMarkerDir;
    std::error_code ec;
    // The container paths that previous runs copied (a marker holds the run number and the path), and whether a path is
    // one of them, lies inside one (the loaders' /deps/fpm holds the players' /deps/fpm/include) or is a directory made
    // around one (/deps/fpm for /deps/fpm/include).
    std::vector<std::string> copied;
    for (fs::directory_iterator it(markers, ec), end; !ec && it != end; it.increment(ec)) {
        std::vector<std::string> lines = text_lines(read_file(it->path()));
        if (lines.size() >= 2) copied.push_back(lines[1]);
    }
    ec.clear();
    auto made_by_a_previous_run = [&](const std::string& path) {
        for (const std::string& other : copied) {
            if (other == path || starts_with(path, other + "/") || starts_with(other, path + "/")) return true;
        }
        return false;
    };
    for (const Copy& copy : copies) {
        bool exists = fs::exists(fs::symlink_status(copy.destination, ec));
        if (exists && !made_by_a_previous_run(copy.path)) {
            throw DcDeterminismError("refusing to replace " + copy.path + " in this container: it exists and was not created by a "
                                     "previous in-container run of `prebuild` (it has no " + std::string(kContainerMarkerDir) +
                                     " marker); nothing was changed");
        }
    }
    fs::create_directories(markers, ec);
    static std::uint64_t run_number = 0;  // a marker says which run copied the path last
    ++run_number;
    for (Copy& copy : copies) {
        fs::remove_all(copy.destination, ec);
        fs::create_directories(copy.destination.parent_path(), ec);
        if (copy.verified) copy.expected_hash = tree_sha256_lf(copy.source);
        if (copy.directory) {
            fs::create_directories(copy.destination, ec);
            fs::copy(copy.source, copy.destination, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        } else {
            fs::copy_file(copy.source, copy.destination, fs::copy_options::overwrite_existing, ec);
        }
        if (ec) {
            throw DcDeterminismError("cannot copy " + native(copy.source) + " to " + copy.path + " in this container: " + ec.message());
        }
        write_bytes(markers / marker_name(copy.path), std::to_string(run_number) + "\n" + copy.path + "\n");
        if (!copy.read_only) make_tree_writable(copy.destination);
    }

    std::vector<std::string> argv = in_container_command(environment);
    ToolOutput result = prebuild_hooks().container_process ? prebuild_hooks().container_process(argv, echo)
                                                           : run_streamed(argv, echo, false);
    if (result.exit_code != 0) {
        throw DcDeterminismError(what + " failed (exit code " + std::to_string(result.exit_code) + "):\n" + tail(result.output, 40));
    }
    for (const Copy& copy : copies) {
        if (copy.verified && tree_sha256_lf(copy.destination) != copy.expected_hash) {
            throw DcDeterminismError(what + ": the read-only copy of " + copy.path + " was changed by the build script");
        }
    }
    for (const Copy& copy : copies) {
        if (copy.read_only) continue;
        fs::copy(copy.destination, copy.source, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            throw DcDeterminismError("cannot copy " + copy.path + " back to " + native(copy.source) + ": " + ec.message());
        }
    }
    return result;
}

// Runs one build script in the image: `docker run` or, inside a container, locally (same script, environment and mounts).
// The script is written below `scratch` first (LF line endings). `as_user`: the Docker executor runs it as the
// calling user (the outputs belong to the user); the in-container executor always runs as the build user.
ToolOutput run_script(const fs::path& scratch, const std::string& what, const std::string& image, const std::vector<Mount>& mounts,
                      const std::vector<std::pair<std::string, std::string>>& environment, const std::string& script_name,
                      const std::string& script_text, bool as_user, bool echo) {
    fs::path file = scratch / script_name;
    write_script(file, script_text);
    if (executor().container) return run_in_container(what, mounts, environment, file, echo);
    return checked_docker(what, docker_run_args(image, mounts, environment, file, as_user), echo);
}

Json read_manifest(const fs::path& root);

// --- the fixed build directory ----------------------------------------------------------------------------
//
// Every build reads a copy of the sources at ONE fixed absolute path (common/scratch.hpp) and writes below it, so that
// the bytes of an artifact do not depend on where the repository is checked out (MSVC derives names embedded in the
// binary from the source path), and a build never writes into the repository. The copy is verified against the
// checkout; the directory is removed when the run ends and two runs never overlap.

class BuildScratch {
public:
    explicit BuildScratch(const fs::path& root)
        : directory_(prebuild_hooks().scratch_path.empty() ? default_scratch_path() : prebuild_hooks().scratch_path) {
        std::error_code ec;
        for (const char* relative : {"src/vendor", "src/worker"}) {
            fs::path from = root / path_from_utf8(relative);
            fs::path to = directory_.path() / path_from_utf8(relative);
            fs::create_directories(to.parent_path(), ec);
            fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) throw DcDeterminismError(std::string("cannot copy ") + relative + " to " + native(to) + ": " + ec.message());
            if (tree_sha256_lf(to) != tree_sha256_lf(from)) {
                throw DcDeterminismError(std::string("the copy of ") + relative + " in " + native(directory_.path()) +
                                 " differs from the checkout");
            }
        }
        // The toolchain script the recipe runs below the root it is given (the synthetic roots of the tests have none).
        fs::path script = root / "scripts" / "build-local.ps1";
        if (fs::is_regular_file(script, ec)) {
            fs::path to = directory_.path() / "scripts" / "build-local.ps1";
            fs::create_directories(to.parent_path(), ec);
            fs::copy_file(script, to, fs::copy_options::overwrite_existing, ec);
            if (ec) throw DcDeterminismError("cannot copy " + native(script) + " to " + native(to) + ": " + ec.message());
        }
        fs::create_directories(path(), ec);
        if (ec) throw DcDeterminismError("cannot create " + native(path()) + ": " + ec.message());
    }

    // The root of the copied sources: what the builds take as the repository root.
    const fs::path& root() const { return directory_.path(); }
    // Where the builds stage their results.
    fs::path path() const { return directory_.path() / "out"; }

private:
    ScratchDirectory directory_;
};

// --- the toolchain ----------------------------------------------------------------------------------------
//
// What a build depends on besides the sources, recorded in the manifests and compared first by `--check`:
// `toolchain.windows-amd64.{msvc,gcc,clang}` (the version facts of the build script's probe), and for Docker builds
// `{image, image_id}` (`toolchain.linux-amd64`, `toolchain.wasm`).

const char* const kWindowsKey = "windows-amd64";
const char* const kLinuxKey = "linux-amd64";
const char* const kWasmKey = "wasm";

Json docker_toolchain(const std::string& image, const std::string& image_id) {
    Json out = Json::object();
    out["image"] = image;
    out["image_id"] = image_id;
    return out;
}

// The version facts of the Windows compilers (only gcc with `gcc_only`: the worker-only mode builds nothing else).
Json windows_toolchain(const fs::path& root, bool gcc_only) {
    Json out = Json::object();
    for (const char* id : {"msvc", "gcc", "clang"}) {
        if (gcc_only && std::string(id) != "gcc") continue;
        BuildToolchain toolchain = probe_build_toolchain(root, "loader", id);
        if (!toolchain.facts.is_object()) {
            throw DcDeterminismError(std::string("the ") + id + " toolchain probe reported no version facts");
        }
        out[id] = toolchain.facts;
    }
    return out;
}

// `toolchain` of a manifest: `existing` with the members of `update` replaced (windows-amd64: compiler by compiler).
Json merge_toolchain(const Json& existing, const Json& update) {
    Json out = existing.is_object() ? existing : Json::object();
    if (!update.is_object()) return out;
    for (auto it = update.begin(); it != update.end(); ++it) {
        if (it.key() == kWindowsKey && out[it.key()].is_object() && it.value().is_object()) {
            for (auto compiler = it.value().begin(); compiler != it.value().end(); ++compiler) {
                out[it.key()][compiler.key()] = compiler.value();
            }
        } else {
            out[it.key()] = it.value();
        }
    }
    return out;
}

// `merged` with `toolchain` as its member (in place when present, else right after `players`).
Json with_toolchain(const Json& merged, const Json& toolchain) {
    if (merged.contains("toolchain")) {
        Json out = merged;
        out["toolchain"] = toolchain;
        return out;
    }
    Json out = Json::object();
    bool placed = false;
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        out[it.key()] = it.value();
        if (it.key() == "players") {
            out["toolchain"] = toolchain;
            placed = true;
        }
    }
    if (!placed) out["toolchain"] = toolchain;
    return out;
}

std::string join_text(const std::vector<std::string>& items, const std::string& separator) {
    std::string out;
    for (const std::string& item : items) out += (out.empty() ? "" : separator) + item;
    return out;
}

void leaves_of(const Json& value, const std::string& path, std::map<std::string, std::string>& into) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            leaves_of(it.value(), path.empty() ? it.key() : path + "." + it.key(), into);
        }
    } else if (value.is_array()) {
        for (std::size_t i = 0; i < value.size(); ++i) leaves_of(value[i], path + "[" + std::to_string(i) + "]", into);
    } else {
        into[path] = value.is_string() ? value.get<std::string>() : json_dumps(value, DumpOptions::compact());
    }
}

// The leaves in which two JSON values differ: "path: recorded 'x', current 'y'" (a missing leaf is <absent>).
std::vector<std::string> json_differences(const Json& recorded, const Json& current, const std::string& prefix) {
    std::map<std::string, std::string> a;
    std::map<std::string, std::string> b;
    leaves_of(recorded, prefix, a);
    leaves_of(current, prefix, b);
    std::vector<std::string> out;
    for (const auto& item : a) {
        auto other = b.find(item.first);
        std::string now = other == b.end() ? "<absent>" : other->second;
        if (now != item.second) out.push_back(item.first + ": recorded '" + item.second + "', current '" + now + "'");
    }
    for (const auto& item : b) {
        if (!a.count(item.first)) out.push_back(item.first + ": recorded '<absent>', current '" + item.second + "'");
    }
    return out;
}

// The paths alone, at most six, for the one-line report of a manifest.
std::string differing_paths(const std::vector<std::string>& differences) {
    std::vector<std::string> paths;
    for (const std::string& item : differences) paths.push_back(item.substr(0, item.find(": recorded ")));
    std::size_t shown = std::min<std::size_t>(paths.size(), 6);
    std::string out = join_text(std::vector<std::string>(paths.begin(), paths.begin() + shown), ", ");
    if (paths.size() > shown) out += ", +" + std::to_string(paths.size() - shown) + " more";
    return out;
}

// Each member of `current` against what `recorded` has under that key: "toolchain not recorded: ..." or
// "toolchain differs: ..." (the toolchain is compared before anything is built).
void compare_toolchain(std::vector<std::string>& problems, const std::string& where, const Json& recorded, const Json& current) {
    for (auto it = current.begin(); it != current.end(); ++it) {
        std::string at = where + "." + it.key();
        Json member = json_get(recorded, it.key().c_str());
        if (member.is_null() || (member.is_object() && member.empty())) {
            problems.push_back("toolchain not recorded: " + at);
            continue;
        }
        std::vector<std::string> differences = json_differences(member, it.value(), at);
        if (!differences.empty()) problems.push_back("toolchain differs: " + join_text(differences, "; "));
    }
}

// An installed JSON file, or null when it is missing.
Json read_installed_json(const fs::path& root, const std::string& relative) {
    std::error_code ec;
    fs::path path = root / path_from_utf8(relative);
    if (!fs::is_regular_file(path, ec)) return Json(nullptr);
    return read_json(path);
}

// The toolchain facts of the manifests a run reads against the current ones. `current`: {windows-amd64, linux-amd64, wasm}
// as far as the run builds them; `main_manifest`, `loaders` and `wasm_manifest` say which installed manifests are read.
std::vector<std::string> toolchain_problems(const fs::path& root, const Json& current, bool main_manifest, bool loaders,
                                            bool wasm_manifest_read) {
    std::vector<std::string> problems;
    Json wasm_manifest = read_installed_json(root, std::string(kWasmBundleRel) + "/manifest.json");
    if (main_manifest) {
        Json recorded = json_get(read_manifest(root), "toolchain");
        std::string where = std::string(kManifestRel) + " toolchain";
        for (auto it = current.begin(); it != current.end(); ++it) {
            Json one = Json::object();
            one[it.key()] = it.value();
            Json recorded_here = recorded;
            if (it.key() == kWindowsKey && it.value().is_object() && json_get(recorded, kWindowsKey).is_object()) {
                // The worker-only mode probes gcc alone: compare the compilers the run has, not the whole member.
                Json subset = Json::object();
                for (auto compiler = it.value().begin(); compiler != it.value().end(); ++compiler) {
                    Json have = json_get(json_get(recorded, kWindowsKey), compiler.key().c_str());
                    if (have.is_null()) {
                        problems.push_back("toolchain not recorded: " + where + "." + kWindowsKey + "." + compiler.key());
                    } else {
                        subset[compiler.key()] = have;
                    }
                }
                if (subset.empty()) continue;
                recorded_here = Json::object();
                recorded_here[kWindowsKey] = subset;
                one[kWindowsKey] = Json::object();
                for (auto compiler = subset.begin(); compiler != subset.end(); ++compiler) one[kWindowsKey][compiler.key()] = it.value()[compiler.key()];
            }
            compare_toolchain(problems, where, recorded_here, one);
        }
    }
    if (loaders) {
        if (current.contains(kWindowsKey)) {
            Json windows = read_installed_json(root, "prebuilt/loaders/windows-amd64/manifest.json");
            std::string windows_where = "prebuilt/loaders/windows-amd64/manifest.json toolchain";
            if (windows.is_null()) {
                problems.push_back("toolchain not recorded: prebuilt/loaders/windows-amd64/manifest.json is missing");
            } else {
                compare_toolchain(problems, windows_where, json_get(windows, "toolchain"), json_get(current, kWindowsKey));
            }
        }
        for (const char* platform : {kLinuxKey, kArm64Key}) {
            if (!current.contains(platform)) continue;
            std::string relative = std::string("prebuilt/loaders/") + platform + "/manifest.json";
            Json linux_loaders = read_installed_json(root, relative);
            std::string recorded_image = member_text(linux_loaders, {"image_id"});
            if (recorded_image.empty()) {
                problems.push_back("toolchain not recorded: " + relative + " has no image_id");
            } else if (recorded_image != member_text(current, {platform, "image_id"})) {
                problems.push_back("toolchain differs: " + relative + " image_id: recorded '" + recorded_image + "', current '" +
                                   member_text(current, {platform, "image_id"}) + "'");
            }
        }
    }
    if (wasm_manifest_read) {
        std::string recorded_image = member_text(wasm_manifest, {"toolchain", "image_id"});
        std::string current_image = member_text(current, {kWasmKey, "image_id"});
        if (recorded_image.empty()) {
            problems.push_back(std::string("toolchain not recorded: ") + kWasmBundleRel + "/manifest.json has no toolchain.image_id");
        } else if (recorded_image != current_image) {
            problems.push_back(std::string("toolchain differs: ") + kWasmBundleRel + "/manifest.json toolchain.image_id: recorded '" +
                               recorded_image + "', current '" + current_image + "'");
        }
    }
    return problems;
}

// --- the check ---------------------------------------------------------------------------------------------
//
// `--check` builds everything the mode builds into the scratch directory, installs nothing and compares: every artifact
// with the installed file (identical or differs, and for a differing PE or ELF file whether its code sections are
// identical), every manifest with the one the rebuild would write, member by member (not byte by byte).

// One line of the report: to the hook of the tests, else to standard output.
void report_line(const std::string& line) {
    if (prebuild_hooks().report) {
        prebuild_hooks().report(line);
        return;
    }
    std::string text = line + "\n";
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

struct CheckReport {
    std::vector<std::string> lines;
    bool differs = false;

    void add(const std::string& line) { lines.push_back(line); }
    int exit_code() const { return differs ? 1 : 0; }
    void print() const {
        for (const std::string& item : lines) report_line(item);
    }
};

struct CheckedFile {
    std::string name;
    fs::path installed;
    fs::path rebuilt;
};

// One artifact: its first file is the binary, the others are its sidecars.
void check_artifact(CheckReport& report, const std::string& label, const std::vector<CheckedFile>& files) {
    std::vector<std::string> different;
    std::string main_installed;
    std::string main_rebuilt;
    bool main_present = false;
    std::error_code ec;
    for (std::size_t i = 0; i < files.size(); ++i) {
        const CheckedFile& file = files[i];
        std::string rebuilt = read_file(file.rebuilt);
        if (!fs::is_regular_file(file.installed, ec)) {
            different.push_back(file.name + " is not installed");
            continue;
        }
        std::string installed = read_file(file.installed);
        if (installed != rebuilt) {
            different.push_back(file.name + " installed " + sha256_hex(installed).substr(0, 12) + ", rebuilt " +
                                sha256_hex(rebuilt).substr(0, 12));
            if (i == 0) {
                main_present = true;
                main_installed = std::move(installed);
                main_rebuilt = std::move(rebuilt);
            }
        }
    }
    if (different.empty()) {
        report.add(label + ": identical");
        return;
    }
    report.differs = true;
    report.add(label + ": differs (" + join_text(different, "; ") + ")");
    if (main_present) {
        CodeComparison comparison = compare_code_sections(main_installed, main_rebuilt);
        if (comparison.comparable) report.add(label + ": " + code_comparison_text(comparison));
    }
}

// A manifest: the rebuilt one (`rebuilt`) against the installed one (null: not installed), member by member.
void check_manifest(CheckReport& report, const std::string& label, const Json& installed, const Json& rebuilt) {
    if (installed.is_null()) {
        report.differs = true;
        report.add("manifest " + label + ": differs (not installed)");
        return;
    }
    std::vector<std::string> differences = json_differences(installed, rebuilt, "");
    if (differences.empty()) {
        report.add("manifest " + label + ": identical");
    } else {
        report.differs = true;
        report.add("manifest " + label + ": differs (" + differing_paths(differences) + ")");
    }
}

// The members of `value` whose key starts with one of the prefixes (the entries of the platforms a host builds).
Json entries_of(const Json& value, const std::vector<std::string>& prefixes) {
    if (!value.is_object()) return value;
    Json out = Json::object();
    for (auto it = value.begin(); it != value.end(); ++it) {
        for (const std::string& prefix : prefixes) {
            if (starts_with(it.key(), prefix)) {
                out[it.key()] = it.value();
                break;
            }
        }
    }
    return out;
}

const std::string kSmokeSamples = std::to_string(kPrebuildSmokeSamples);

struct BuiltBinary {
    fs::path path;
    std::string compiler;
};

// The version line of the compiler as the manifest records it. g++ prints the name it was started by; the probe
// finds it with its PATHEXT spelling (`g++.EXE`), so it is started as `<bin dir>/g++.exe` here, and the probe's
// own line is the fallback.
std::string windows_compiler_version(const BuildToolchain& toolchain) {
    if (toolchain.id != "gcc" || toolchain.bin_dir.empty()) return toolchain.version;
    ToolOutput result = run_program({native(path_from_utf8(toolchain.bin_dir) / "g++.exe"), "--version"}, false);
    std::string line = result.exit_code == 0 ? first_line(result.output) : std::string();
    return line.empty() ? toolchain.version : line;
}

// The reproducibility flags of a Windows build (localbuild: /Brepro; -ffile-prefix-map and --no-insert-timestamp), and
// as the manifests record them: the fixed build path written as <repo>, an msvc linker part after `/link`.
std::string recorded_text(std::string flag, const fs::path& build_root) {
    std::string root = to_posix(native(build_root));
    for (std::size_t at = flag.find(root); at != std::string::npos; at = flag.find(root, at + 6)) flag.replace(at, root.size(), "<repo>");
    return flag;
}

std::vector<std::string> recorded_flags(std::vector<std::string> base, const std::string& compiler_id, const fs::path& build_root) {
    ReproducibleFlags reproducible = reproducible_flags(compiler_id, build_root);
    for (const std::string& flag : reproducible.compile) base.push_back(recorded_text(flag, build_root));
    if (!reproducible.link.empty() && compiler_id == "msvc") base.push_back("/link");
    for (const std::string& flag : reproducible.link) base.push_back(recorded_text(flag, build_root));
    return base;
}

BuiltBinary build_windows_worker(const fs::path& root, const fs::path& scratch, const std::string& source_sha) {
    BuildToolchain gcc = probe_build_toolchain(root, "player", "gcc");
    fs::path rsp = scratch / "defs.rsp";
    // The backslash-escaped quotes make the macro a string literal.
    write_bytes(rsp, "-DDC_WORKER_SOURCE_SHA256=\\\"" + source_sha + "\\\"\n");
    fs::path exe = scratch / "dc_worker.exe";
    std::vector<std::string> command = {gcc.cxx};
    for (const std::string& flag : {"-std=c++17", "-O2", "-static", "-s"}) command.push_back(flag);
    ReproducibleFlags reproducible = reproducible_flags("gcc", root);
    for (const std::string& flag : reproducible.compile) command.push_back(flag);
    for (const std::string& flag : reproducible.link) command.push_back(flag);
    command.push_back("@" + native(rsp));
    command.push_back(native(root / "src" / "worker" / "worker.cpp"));
    command.push_back("-o");
    command.push_back(native(exe));
    command.push_back("-lshell32");
    run_build_tool(root, "windows g++ build of dc_worker", "player", "gcc", fs::path(), command);
    // Static linking check: the binary runs without the MSYS2 directories on PATH.
    std::string self_test = first_line(checked_program("windows --self-test", {native(exe), "--self-test"}, true));
    if (self_test != "OK") throw DcDeterminismError("windows self-test printed: " + self_test);
    std::string version = first_line(checked_program("windows --version", {native(exe), "--version"}, true));
    if (version != "dc_worker " + source_sha) throw DcDeterminismError("windows --version mismatch: " + version);
    log("windows self-test (MSYS2 removed from PATH): " + self_test + "; version: " + version);
    return BuiltBinary{exe, windows_compiler_version(gcc)};
}

BuiltBinary build_linux_worker(const fs::path& root, const fs::path& scratch, const std::string& image,
                               const std::string& source_sha, const LinuxTarget& target) {
    fs::path out = scratch / "linux";
    std::error_code ec;
    fs::create_directories(out, ec);
    make_world_writable(out);
    ToolOutput result = run_script(scratch, "docker build of the linux dc_worker", image, {{root, "/src:ro"}, {out, "/out"}},
                                   {{"SOURCE_SHA", source_sha}}, "linux-build.sh", linux_worker_script(target.arm64()), true, true);
    std::vector<std::string> lines = text_lines(result.output);
    if (!has_line(lines, "self-test: OK")) throw DcDeterminismError("linux self-test did not print OK");
    if (!has_line(lines, "version: dc_worker " + source_sha)) throw DcDeterminismError("linux --version mismatch");
    return BuiltBinary{out / "dc_worker", py_strip(read_file(out / "compiler.txt"))};
}

// One player binary: where it was built and its manifest entry.
struct PlayerBuild {
    std::string key;     // windows-amd64-msvc/normal_dist
    std::string dir;     // windows-amd64-msvc
    std::string player;  // normal_dist
    std::string file;    // worker.exe, worker or worker.js
    fs::path built;
    fs::path built_wasm;  // wasm only
    Json entry;
};

// Everything the player builds produce.
struct PlayerSet {
    std::vector<PlayerBuild> builds;
    std::vector<std::pair<std::string, std::vector<SmokeRun>>> smoke;  // player -> outputs

    PlayerSet() {
        for (const char* player : kPlayers) smoke.emplace_back(player, std::vector<SmokeRun>());
    }
    void add_smoke(const std::string& player, const std::string& label, const fs::path& path) {
        for (auto& item : smoke) {
            if (item.first == player) item.second.push_back(SmokeRun{label, path});
        }
    }
};

std::string player_path(const std::string& dir, const std::string& player, const std::string& file) {
    return "prebuilt/players/" + dir + "/" + player + "/" + file;
}

void build_windows_players(const fs::path& root, const fs::path& scratch, PlayerSet& set) {
    fs::path players = scratch / "players";
    fs::path smoke = scratch / "smoke";
    std::error_code ec;
    fs::create_directories(smoke, ec);

    struct Tool {
        const char* id;
        const char* target;
    };
    for (const Tool& tool : {Tool{"msvc", "windows-msvc"}, Tool{"gcc", "windows-gcc"}, Tool{"clang", "windows-clang"}}) {
        std::string id = tool.id;
        BuildToolchain toolchain = probe_build_toolchain(root, "player", id);
        std::string compiler = windows_compiler_version(toolchain);
        for (const char* player_name : kPlayers) {
            std::string player = player_name;
            std::string dir_name = "windows-amd64-" + id;
            fs::path dir = players / dir_name / player;
            fs::create_directories(dir, ec);
            fs::path exe = dir / "worker.exe";
            std::vector<std::string> flags = prebuilt_player_flags(tool.target, player);
            ReproducibleFlags reproducible = reproducible_flags(id, root);
            std::pair<std::vector<fs::path>, std::vector<fs::path>> inputs = player_inputs(root, player);
            std::vector<std::string> command = {toolchain.cxx};
            command.insert(command.end(), flags.begin(), flags.end());
            command.insert(command.end(), reproducible.compile.begin(), reproducible.compile.end());
            if (id == "msvc") {
                for (const fs::path& include : inputs.first) command.push_back("/I" + native(include));
                for (const fs::path& source : inputs.second) command.push_back(native(source));
                command.push_back("/Fe:" + native(exe));
                command.push_back("/Fo:" + native(dir) + "\\");
                if (!reproducible.link.empty()) command.push_back("/link");
                command.insert(command.end(), reproducible.link.begin(), reproducible.link.end());
            } else {
                command.insert(command.end(), reproducible.link.begin(), reproducible.link.end());
                for (const fs::path& include : inputs.first) {
                    command.push_back("-I");
                    command.push_back(native(include));
                }
                for (const fs::path& source : inputs.second) command.push_back(native(source));
                command.push_back("-o");
                command.push_back(native(exe));
            }
            log("building player " + player + " with " + id);
            run_build_tool(root, id + " build of " + player, "player", id, id == "msvc" ? dir : fs::path(), command);
            if (!fs::is_regular_file(exe, ec)) throw DcDeterminismError("the player worker was not created: " + native(exe));
            PlayerBuild build;
            build.key = dir_name + "/" + player;
            build.dir = dir_name;
            build.player = player;
            build.file = "worker.exe";
            build.built = exe;
            build.entry = player_entry_json(player_path(dir_name, player, "worker.exe"), sha256_file(exe), "windows-amd64", id,
                                            compiler, player, recorded_flags(flags, id, root), {});
            fs::path output = smoke / (dir_name + "-" + player + ".bin");
            // The MSYS2 directories are removed from PATH: the binary must not need any MSYS2 DLL.
            checked_program("smoke " + id + " " + player, {native(exe), native(output), kSmokeSamples}, id != "msvc");
            set.add_smoke(player, dir_name, output);
            set.builds.push_back(std::move(build));
        }
    }
}

fs::path player_library(const fs::path& root) { return root / path_from_utf8(kPlayerLibraryRel); }

std::vector<Mount> player_mounts(const fs::path& root, const fs::path& out) {
    return {{root, "/src:ro"},
            {player_library(root), "/player:ro"},
            {root / "src" / "vendor" / "fpm" / "include", "/deps/fpm/include:ro"},
            {root / "src" / "vendor" / "nlohmann-json" / "include", "/deps/json/include:ro"},
            {out, "/out"}};
}

void build_linux_players(const fs::path& root, const fs::path& scratch, const std::string& image, bool echo,
                         const LinuxTarget& target, PlayerSet& set) {
    fs::path out = scratch / "linux-players";
    std::error_code ec;
    fs::create_directories(out, ec);
    make_world_writable(out);
    std::string ids;
    for (const std::string& id : target.compilers()) ids += (ids.empty() ? "" : ", ") + id;
    log("building " + target.platform + " players (" + ids + ")");
    run_script(scratch, "docker linux players", image, player_mounts(root, out), {}, "linux-players.sh",
               linux_players_script(target.arm64()), true, echo);
    std::string image_id = image_id_of(image);
    for (const std::string& id : target.compilers()) {
        std::string base = id.compare(0, 3, "gcc") == 0 ? "gcc" : "clang";
        std::string compiler = py_strip(read_file(out / (base + ".txt")));
        for (const char* player_name : kPlayers) {
            std::string player = player_name;
            std::string dir_name = target.platform + "-" + id;
            fs::path built = out / dir_name / player / "worker";
            if (!fs::is_regular_file(built, ec)) {
                throw DcDeterminismError("the linux player worker was not created: " + native(built));
            }
            PlayerBuild build;
            build.key = dir_name + "/" + player;
            build.dir = dir_name;
            build.player = player;
            build.file = "worker";
            build.built = built;
            build.entry = player_entry_json(player_path(dir_name, player, "worker"), sha256_file(built), target.platform, id,
                                            compiler, player, prebuilt_player_flags("linux-" + id, player),
                                            {{"image_id", image_id}, {"image", image}});
            set.add_smoke(player, dir_name, out / "smoke" / (dir_name + "-" + player + ".bin"));
            set.builds.push_back(std::move(build));
        }
    }
}

void reclaim_ownership(const std::string& image, const fs::path& directory);

void build_wasm_players(const fs::path& root, const fs::path& scratch, const std::string& emsdk_image, PlayerSet& set) {
    fs::path out = scratch / "wasm";
    std::error_code ec;
    fs::create_directories(out, ec);
    log("building wasm players in Docker");
    try {
        run_script(scratch, "docker wasm players", emsdk_image, player_mounts(root, out), {}, "wasm-build.sh", wasm_players_script(),
                   false, true);
    } catch (const DcDeterminismError&) {
        reclaim_ownership(emsdk_image, out);
        throw;
    }
    // The root of the container wrote the files: hand them to the calling user (a Linux host) so that the scratch
    // directory can be removed.
    reclaim_ownership(emsdk_image, out);
    std::string emsdk_id = image_id_of(emsdk_image);
    std::string em_version = py_strip(read_file(out / "em.txt"));
    std::string container_node = py_strip(read_file(out / "node.txt"));
    std::string host_node;
    try {
        host_node = first_line(checked_program("node --version", {"node", "--version"}, false));
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("node is required on PATH for the wasm smoke test:") + error.what());
    }
    log("node: container " + container_node + ", host " + host_node);
    fs::path smoke = scratch / "smoke";
    fs::create_directories(smoke, ec);
    for (const char* player_name : kPlayers) {
        std::string player = player_name;
        fs::path js = out / "wasm" / player / "worker.js";
        fs::path wasm = out / "wasm" / player / "worker.wasm";
        if (!fs::is_regular_file(js, ec) || !fs::is_regular_file(wasm, ec)) {
            throw DcDeterminismError("the wasm player was not created: " + native(js));
        }
        Json sidecars = Json::object();
        Json wasm_entry = Json::object();
        wasm_entry["path"] = player_path("wasm", player, "worker.wasm");
        wasm_entry["sha256"] = sha256_file(wasm);
        sidecars["worker.wasm"] = wasm_entry;
        PlayerBuild build;
        build.key = "wasm/" + player;
        build.dir = "wasm";
        build.player = player;
        build.file = "worker.js";
        build.built = js;
        build.built_wasm = wasm;
        build.entry = player_entry_json(player_path("wasm", player, "worker.js"), sha256_file(js), "wasm", "wasm", em_version,
                                        player, prebuilt_player_flags("wasm", player),
                                        {{"image", emsdk_image}, {"image_id", emsdk_id}, {"sidecars", sidecars}});
        set.add_smoke(player, "wasm-container", out / "smoke" / ("wasm-container-" + player + ".bin"));
        fs::path host_output = smoke / ("wasm-host-" + player + ".bin");
        checked_program("smoke wasm host " + player, {"node", native(js), native(host_output), kSmokeSamples}, false);
        set.add_smoke(player, "wasm-host-node", host_output);
        set.builds.push_back(std::move(build));
    }
}

// --- the WebAssembly dc_worker -----------------------------------------------------------------------------

// A built bundle: the files in the scratch directory and the manifest that describes them.
struct WasmWorkerBuild {
    fs::path js;
    fs::path wasm;
    std::string js_sha256;
    std::string wasm_sha256;
    Json manifest;
};

// A built program that reads `input` from its stdin and must succeed; its stdout is returned.
std::string checked_program_input(const std::string& what, const std::vector<std::string>& argv, const std::string& input) {
    ToolOutput result;
    if (prebuild_hooks().program_input) {
        result = prebuild_hooks().program_input(argv, input);
    } else {
        StartOptions options;
        options.argv = argv;
        RunResult run = run_capture(options, input, 0);
        result.exit_code = run.exit_code;
        result.output = run.exit_code == 0 ? run.out : run.out + run.err;
    }
    if (result.exit_code != 0) {
        throw DcDeterminismError(what + " failed (exit code " + std::to_string(result.exit_code) + "):\n" + tail(result.output, 40));
    }
    return result.output;
}

// A bind-mounted directory written by a root container belongs to root on a Linux host: hand it to the calling user
// so that the scratch directory can be removed. (Docker Desktop on Windows maps the ownership by itself.)
void reclaim_ownership(const std::string& image, const fs::path& directory) {
    std::string user = docker_user();
    if (user.empty()) return;
    try {
        run_docker({"run", "--rm", "--entrypoint", "chown", "-v", native(directory) + ":/out", image, "-R", user, "/out"}, false);
    } catch (const DcDeterminismError&) {
    }
}

// A result line of the worker without its last field (the wall time of the simulation, not part of the comparison).
std::string without_last_field(const std::string& line) {
    std::vector<std::string> fields;
    std::string current;
    for (char c : py_strip(line)) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) fields.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) fields.push_back(current);
    if (!fields.empty()) fields.pop_back();
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) out += (i == 0 ? "" : " ") + fields[i];
    return out;
}

// Builds the wasm dc_worker in the Emscripten container, smoke-tests it there and with the host node (the results
// must agree) and describes it. Nothing under `root` is written.
WasmWorkerBuild build_wasm_worker(const fs::path& root, const fs::path& scratch, const std::string& emsdk_image,
                                  const WasmSources& sources) {
    std::string image_id = image_id_of(emsdk_image);
    ToolOutput digests = checked_docker("docker image inspect " + emsdk_image,
                                        {"image", "inspect", "--format", "{{json .RepoDigests}}", emsdk_image}, false);
    WasmWorkerFacts facts;
    facts.image = emsdk_image;
    facts.image_id = image_id;
    Json repo_digests = parse_json_text(first_line(digests.output), "docker image inspect " + emsdk_image);
    if (repo_digests.is_array()) {
        for (const Json& digest : repo_digests) {
            if (digest.is_string() && !digest.get<std::string>().empty()) facts.repo_digests.push_back(digest.get<std::string>());
        }
    }
    log("docker image: " + emsdk_image + " (" + image_id + ")");

    fs::path out = scratch / "wasm-worker";
    std::error_code ec;
    fs::create_directories(out, ec);
    log("building the wasm worker in Docker");
    try {
        checked_docker("docker wasm worker build",
                       {"run", "--rm", "--entrypoint", "bash", "-v", native(root) + ":/repo:ro", "-v", native(out) + ":/out",
                        emsdk_image, kWasmScriptInContainer},
                       true);
    } catch (const DcDeterminismError&) {
        reclaim_ownership(emsdk_image, out);
        throw;
    }
    reclaim_ownership(emsdk_image, out);

    fs::path js = out / "dc_worker.js";
    fs::path wasm = out / "dc_worker.wasm";
    Json info = read_json(out / "build-info.json");
    if (member_text(info, {"schema"}) != kWasmBuildInfoSchema) {
        throw DcDeterminismError("unexpected build-info schema: " + member_text(info, {"schema"}));
    }
    if (member_text(info, {"worker_source_sha256_lf"}) != sources.worker_sha256) {
        throw DcDeterminismError("the container built another worker.cpp than the host verified");
    }
    std::string expected_version = "dc_worker " + sources.worker_sha256;
    std::string container_version = member_text(info, {"smoke", "version"});
    std::string container_self_test = member_text(info, {"smoke", "self_test"});
    if (container_version != expected_version || container_self_test != "OK") {
        throw DcDeterminismError("container smoke test: version '" + container_version + "', self-test '" + container_self_test + "'");
    }

    // Host smoke test: the same checks with the host node; the results must equal the container's.
    try {
        facts.host_node = first_line(checked_program("node --version", {"node", "--version"}, false));
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("node is required on PATH for the host smoke test: ") + error.what());
    }
    log("node: container " + member_text(info, {"toolchain", "node"}) + ", host " + facts.host_node);
    std::string script = native(js);
    std::string module = native(wasm);
    std::string host_version = first_line(checked_program("host --version", {"node", script, "--version"}, false));
    std::string host_self_test = first_line(checked_program("host --self-test", {"node", script, "--self-test"}, false));
    if (host_version != expected_version || host_self_test != "OK") {
        throw DcDeterminismError("host smoke test: version '" + host_version + "', self-test '" + host_self_test + "'");
    }
    fs::path smoke_input = root / path_from_utf8(kWasmSmokeInputRel);
    if (sha256_file(smoke_input) != member_text(info, {"smoke_input_sha256"})) {
        throw DcDeterminismError(std::string(kWasmSmokeInputRel) + " changed during the build");
    }
    std::string shot_ids;
    for (int shot = 0; shot < kWasmSmokeShots; ++shot) shot_ids += std::to_string(shot) + "\n";
    facts.host_seconds_per_frame = Json::object();
    facts.host_shots = Json::object();
    for (const char* engine_name : kWasmEngines) {
        std::string engine = engine_name;
        std::string spf = first_line(checked_program("host --seconds-per-frame " + engine,
                                                     {"node", script, "--loader", module, "--engine", engine, "--seconds-per-frame"},
                                                     false));
        std::string shots = checked_program_input("host shots " + engine,
                                                  {"node", script, "--loader", module, "--engine", engine, "--input",
                                                   native(smoke_input), "--max-steps", "60000"},
                                                  shot_ids);
        Json lines = Json::array();
        for (const std::string& line : text_lines(shots)) lines.push_back(without_last_field(line));
        std::string container_spf = member_text(info, {"smoke", "seconds_per_frame", engine_name});
        if (spf != container_spf) {
            throw DcDeterminismError("seconds per frame of " + engine + " differs: host " + spf + ", container " + container_spf);
        }
        if (lines != json_get(json_get(json_get(info, "smoke"), "shots"), engine_name)) {
            throw DcDeterminismError("the " + engine + " smoke shots of the host node differ from the container's");
        }
        facts.host_seconds_per_frame[engine] = spf;
        facts.host_shots[engine] = lines;
    }
    log("smoke tests agree (container node and host node)");

    WasmWorkerBuild build;
    build.js = js;
    build.wasm = wasm;
    build.js_sha256 = sha256_file(js);
    build.wasm_sha256 = sha256_file(wasm);
    log("dc_worker.js   sha256: " + build.js_sha256);
    log("dc_worker.wasm sha256: " + build.wasm_sha256);
    facts.js_sha256 = build.js_sha256;
    facts.wasm_sha256 = build.wasm_sha256;
    facts.build_info = info;
    build.manifest = wasm_worker_manifest(sources, facts);
    return build;
}

// Whether the installed bundle holds the bytes that were just built (nullopt: there is no installed bundle).
std::optional<bool> wasm_bundle_reproduced(const fs::path& root, const WasmWorkerBuild& build) {
    fs::path directory = root / path_from_utf8(kWasmBundleRel);
    std::error_code ec;
    if (!fs::is_regular_file(directory / "dc_worker.js", ec) || !fs::is_regular_file(directory / "dc_worker.wasm", ec)) {
        return std::nullopt;
    }
    bool same = sha256_file(directory / "dc_worker.js") == build.js_sha256 &&
                sha256_file(directory / "dc_worker.wasm") == build.wasm_sha256;
    log(same ? "the rebuilt bundle is byte-identical to the installed one"
             : "WARNING: the rebuilt bundle differs from the installed one");
    return same;
}

// Copies the bundle below prebuilt/worker/wasm and checks the installed bytes.
void install_wasm_worker(const fs::path& root, const WasmWorkerBuild& build) {
    fs::path directory = root / path_from_utf8(kWasmBundleRel);
    std::error_code ec;
    fs::create_directories(directory, ec);
    copy_replacing(build.js, directory / "dc_worker.js");
    copy_replacing(build.wasm, directory / "dc_worker.wasm");
    write_replacing(directory / "manifest.json", manifest_text(build.manifest));
    if (sha256_file(directory / "dc_worker.js") != build.js_sha256 ||
        sha256_file(directory / "dc_worker.wasm") != build.wasm_sha256) {
        throw DcDeterminismError("installed wasm dc_worker differs from the built one");
    }
    log("installed " + native(directory) + " (dc_worker.js, dc_worker.wasm, manifest.json)");
}

// Decodes and compares the smoke outputs; prints the report; throws when a run is bad.
void verify_smoke(const PlayerSet& set) {
    SmokeReport report = check_smoke(set.smoke);
    for (const std::string& line : report.lines) {
        std::fwrite((line + "\n").data(), 1, line.size() + 1, stdout);
    }
    std::fflush(stdout);
    if (report.failed) throw DcDeterminismError("the player smoke outputs failed the check (see above)");
}

// Copies the built players below prebuilt/players and checks the installed bytes. `replaced`: the directories that
// are removed first (a directory that is not rebuilt any more); null: none.
void install_players(const fs::path& root, const PlayerSet& set, const std::function<bool(const std::string&)>& replaced) {
    fs::path destination = root / "prebuilt" / "players";
    std::error_code ec;
    if (replaced && fs::is_directory(destination, ec)) {
        std::vector<fs::path> obsolete;
        for (fs::directory_iterator it(destination, ec), end; !ec && it != end; it.increment(ec)) {
            if (replaced(path_to_utf8(it->path().filename()))) obsolete.push_back(it->path());
        }
        if (ec) throw DcDeterminismError("cannot list " + native(destination) + ": " + ec.message());
        for (const fs::path& path : obsolete) {
            fs::remove_all(path, ec);
            if (ec) throw DcDeterminismError("cannot remove " + native(path) + ": " + ec.message());
        }
    }
    for (const PlayerBuild& build : set.builds) {
        fs::path target =
            destination / path_from_utf8(build.dir) / path_from_utf8(build.player) / path_from_utf8(build.file);
        copy_replacing(build.built, target);
        if (build.dir.compare(0, 5, "linux") == 0) make_executable(target);
        if (!build.built_wasm.empty()) copy_replacing(build.built_wasm, target.parent_path() / "worker.wasm");
        if (sha256_file(target) != member_text(build.entry, {"sha256"})) {
            throw DcDeterminismError("installed binary differs: " + member_text(build.entry, {"path"}));
        }
    }
}

Json players_binaries(const PlayerSet& set) {
    Json binaries = Json::object();
    for (const PlayerBuild& build : set.builds) binaries[build.key] = build.entry;
    return binaries;
}

Json read_manifest(const fs::path& root) {
    fs::path path = root / path_from_utf8(kManifestRel);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw DcDeterminismError(std::string(kManifestRel) + " is missing");
    Json manifest = read_json(path);
    if (!manifest.is_object()) throw DcDeterminismError(std::string(kManifestRel) + " is not a JSON object");
    return manifest;
}

void write_manifest(const fs::path& root, const Json& manifest) {
    fs::path path = root / path_from_utf8(kManifestRel);
    write_replacing(path, manifest_text(manifest));
    log("wrote " + native(path));
}

void check_schema(const Json& manifest) {
    std::string schema = member_text(manifest, {"schema"});
    if (schema != kPrebuiltManifestSchema) throw DcDeterminismError("unexpected manifest schema: " + schema);
}

// The `binaries` member of the built dc_worker binaries (described from the scratch files): windows-amd64 when `windows`
// was built, and the Linux platform of the host.
Json describe_workers(const BuiltBinary* windows, const std::vector<std::string>& windows_flags, const BuiltBinary& linux_worker,
                      const LinuxTarget& target, const std::string& image, const std::string& image_id) {
    Json binaries = Json::object();
    if (windows != nullptr) {
        binaries["windows-amd64"] =
            worker_binary_json(kWindowsWorkerRel, sha256_file(windows->path), windows->compiler, windows_flags);
    }
    binaries[target.platform] = worker_binary_json(target.worker_rel(), sha256_file(linux_worker.path), linux_worker.compiler,
                                                   target.arm64() ? linux_arm64_worker_flags() : linux_worker_flags(), image, image_id);
    return binaries;
}

// Copies the built dc_worker binaries and checks the installed bytes against their entries.
void install_workers(const fs::path& root, const BuiltBinary* windows, const BuiltBinary& linux_worker, const LinuxTarget& target,
                     const Json& binaries) {
    fs::path linux_dest = root / path_from_utf8(target.worker_rel());
    copy_replacing(linux_worker.path, linux_dest);
    make_executable(linux_dest);
    if (sha256_file(linux_dest) != member_text(binaries, {target.platform.c_str(), "sha256"})) {
        throw DcDeterminismError("installed dc_worker differs from the built one");
    }
    if (windows != nullptr) {
        fs::path windows_dest = root / path_from_utf8(kWindowsWorkerRel);
        copy_replacing(windows->path, windows_dest);
        if (sha256_file(windows_dest) != member_text(binaries, {"windows-amd64", "sha256"})) {
            throw DcDeterminismError("installed dc_worker differs from the built one");
        }
    }
}

[[noreturn]] void refuse(const std::string& reason) {
    throw DcDeterminismError(reason + "\nA full rebuild of all platforms is required: run `dc_determinism prebuild` on a Windows host.");
}

std::string canonical_json(const Json& value) { return json_dumps(value, DumpOptions::compact(true)); }

// --- the linux-arm64 data a Windows host carries ------------------------------------------------------------
//
// A Windows host cannot rebuild linux-arm64: it carries the entries and files of that platform unchanged while they
// match the new sources, and removes them (after everything else was installed) when they do not.

struct Arm64Stale {
    bool worker = false;   // binaries.linux-arm64 and prebuilt/worker/linux-arm64
    bool players = false;  // players.binaries linux-arm64-* and prebuilt/players/linux-arm64-*
    bool loaders = false;  // prebuilt/loaders/linux-arm64
    std::vector<std::string> reasons;

    bool any() const { return worker || players || loaders; }
};

// `rebuild_players`: the players are rebuilt, so their source identity matters; `loader_ids`: the loaders are rebuilt.
Arm64Stale stale_arm64(const fs::path& root, const Json& existing, const SourceIdentity& identity, bool rebuild_players,
                       const std::optional<std::pair<Json, Json>>& loader_ids) {
    Arm64Stale stale;
    Json binaries = json_get(existing, "binaries");
    if (binaries.is_object() && binaries.contains(kArm64Key)) {
        std::optional<std::string> mismatch = worker_mismatch(existing, identity);
        if (mismatch) {
            stale.worker = true;
            stale.reasons.push_back("the linux-arm64 dc_worker is stale: " + *mismatch);
        }
    }
    Json player_binaries = json_get(json_get(existing, "players"), "binaries");
    bool have_players = false;
    if (player_binaries.is_object()) {
        for (auto it = player_binaries.begin(); it != player_binaries.end(); ++it) {
            have_players = have_players || starts_with(it.key(), kArm64PlayerPrefix);
        }
    }
    if (rebuild_players && have_players) {
        std::optional<std::string> mismatch = players_mismatch(existing, identity);
        if (mismatch) {
            stale.players = true;
            stale.reasons.push_back("the linux-arm64 players are stale: " + *mismatch);
        }
    }
    std::error_code ec;
    fs::path loader_manifest_path = root / "prebuilt" / "loaders" / kArm64Key / "manifest.json";
    if (loader_ids && fs::is_regular_file(loader_manifest_path, ec)) {
        Json old = read_json(loader_manifest_path);
        if (canonical_json(json_get(old, "source")) != canonical_json(loader_ids->first) ||
            canonical_json(json_get(old, "dependencies")) != canonical_json(loader_ids->second)) {
            stale.loaders = true;
            stale.reasons.push_back("the linux-arm64 loaders are stale: another simulator source or dependency");
        }
    }
    return stale;
}

// The members of the manifest an update may change; every other member must survive a merge unchanged.
bool is_updated_member(const std::string& key) {
    return key == "schema" || key == "source" || key == "binaries" || key == "players" || key == "toolchain";
}

bool has_prefix(const std::string& key, const std::vector<std::string>& prefixes) {
    for (const std::string& prefix : prefixes) {
        if (starts_with(key, prefix)) return true;
    }
    return false;
}

bool listed(const std::vector<std::string>& items, const std::string& key) {
    return std::find(items.begin(), items.end(), key) != items.end();
}

// Everything the update does not name must survive the merge unchanged (the entries of the platforms this host does not
// build, the wasm members it carries, the `dc_determinism` section and any member this version does not know), and
// nothing else may appear.
void check_carried_members(const Json& existing, const Json& merged, const ManifestUpdate& update, const std::string& when) {
    auto fail = [&](const std::string& what) {
        throw DcDeterminismError("internal check " + when + " failed: " + what + ". Nothing else was changed after this check.");
    };
    for (auto it = existing.begin(); it != existing.end(); ++it) {
        if (is_updated_member(it.key())) continue;
        if (!merged.contains(it.key()) || merged[it.key()] != it.value()) fail("the " + it.key() + " section changed");
    }
    for (auto it = merged.begin(); it != merged.end(); ++it) {
        if (!is_updated_member(it.key()) && !existing.contains(it.key())) fail("the " + it.key() + " section changed");
    }
    if (update.source.is_null() && json_get(existing, "source") != json_get(merged, "source")) fail("the source member changed");

    Json old_binaries = json_get(existing, "binaries");
    Json new_binaries = json_get(merged, "binaries");
    if (old_binaries.is_object()) {
        for (auto it = old_binaries.begin(); it != old_binaries.end(); ++it) {
            if (update.binaries.contains(it.key())) continue;
            bool kept = new_binaries.is_object() && new_binaries.contains(it.key()) && new_binaries[it.key()] == it.value();
            if (listed(update.removed_binaries, it.key())) {
                if (new_binaries.is_object() && new_binaries.contains(it.key())) fail("binaries." + it.key() + " was not removed");
            } else if (!kept) {
                fail("binaries." + it.key() + " was not kept");
            }
        }
    }
    if (new_binaries.is_object()) {
        for (auto it = new_binaries.begin(); it != new_binaries.end(); ++it) {
            bool old = old_binaries.is_object() && old_binaries.contains(it.key());
            if (!old && !update.binaries.contains(it.key())) fail("binaries." + it.key() + " appeared");
        }
    }

    Json old_players = json_get(json_get(existing, "players"), "binaries");
    Json new_players = json_get(json_get(merged, "players"), "binaries");
    if (update.players_source.is_null() &&
        json_get(json_get(existing, "players"), "source") != json_get(json_get(merged, "players"), "source")) {
        fail("players.source changed");
    }
    if (old_players.is_object()) {
        for (auto it = old_players.begin(); it != old_players.end(); ++it) {
            if (has_prefix(it.key(), update.replaced_player_prefixes)) continue;
            if (!new_players.is_object() || !new_players.contains(it.key()) || new_players[it.key()] != it.value()) {
                fail("players.binaries[" + it.key() + "] was not kept");
            }
        }
    }
    if (new_players.is_object()) {
        for (auto it = new_players.begin(); it != new_players.end(); ++it) {
            bool old = old_players.is_object() && old_players.contains(it.key());
            bool replaced = has_prefix(it.key(), update.replaced_player_prefixes);
            if ((!old && !replaced) || (replaced && !update.player_entries.contains(it.key()))) {
                fail("players.binaries[" + it.key() + "] appeared");
            }
        }
    }

    Json old_toolchain = json_get(existing, "toolchain");
    Json new_toolchain = json_get(merged, "toolchain");
    if (old_toolchain.is_object()) {
        for (auto it = old_toolchain.begin(); it != old_toolchain.end(); ++it) {
            bool present = new_toolchain.is_object() && new_toolchain.contains(it.key());
            if (listed(update.removed_toolchain, it.key())) {
                if (present) fail("toolchain." + it.key() + " was not removed");
            } else if (update.toolchain.contains(it.key())) {
                if (it.key() != kWindowsKey || !it.value().is_object()) continue;
                // windows-amd64 is merged compiler by compiler: the compilers the run did not probe stay.
                for (auto compiler = it.value().begin(); compiler != it.value().end(); ++compiler) {
                    if (json_get(update.toolchain[kWindowsKey], compiler.key().c_str()).is_null() &&
                        json_get(json_get(new_toolchain, kWindowsKey), compiler.key().c_str()) != compiler.value()) {
                        fail("toolchain.windows-amd64." + compiler.key() + " was not kept");
                    }
                }
            } else if (!present || new_toolchain[it.key()] != it.value()) {
                fail("toolchain." + it.key() + " was not kept");
            }
        }
    }
    if (new_toolchain.is_object()) {
        for (auto it = new_toolchain.begin(); it != new_toolchain.end(); ++it) {
            bool old = old_toolchain.is_object() && old_toolchain.contains(it.key());
            if (!old && !update.toolchain.contains(it.key())) fail("toolchain." + it.key() + " appeared");
        }
    }
}

// Before the manifest is written: the test seam, then the carried-member check.
void check_merged(const Json& existing, Json& merged, const ManifestUpdate& update) {
    if (prebuild_hooks().adjust_merged_manifest) prebuild_hooks().adjust_merged_manifest(merged);
    check_carried_members(existing, merged, update, "before writing the manifest");
}

// After the manifest was written: it reads back as written and still carries every member.
void check_written(const fs::path& root, const Json& existing, const Json& merged, const ManifestUpdate& update) {
    Json reread = read_manifest(root);
    if (reread != merged) throw DcDeterminismError("internal check after writing the manifest failed: it does not read back as written");
    check_carried_members(existing, reread, update, "after writing the manifest");
}

// The stale linux-arm64 files go last, when the manifest no longer names them.
void remove_stale_arm64(const fs::path& root, const Arm64Stale& stale) {
    std::error_code ec;
    std::string kinds;
    auto note = [&](const char* kind) { kinds += (kinds.empty() ? "" : ", ") + std::string(kind); };
    if (stale.worker) {
        fs::remove_all(root / path_from_utf8(kArm64WorkerDirRel), ec);
        if (ec) throw DcDeterminismError(std::string("cannot remove ") + kArm64WorkerDirRel + ": " + ec.message());
        note("dc_worker");
    }
    if (stale.players) {
        fs::path players = root / "prebuilt" / "players";
        std::vector<fs::path> directories;
        if (fs::is_directory(players, ec)) {
            for (fs::directory_iterator it(players, ec), end; !ec && it != end; it.increment(ec)) {
                if (starts_with(path_to_utf8(it->path().filename()), kArm64PlayerPrefix)) directories.push_back(it->path());
            }
        }
        for (const fs::path& directory : directories) {
            fs::remove_all(directory, ec);
            if (ec) throw DcDeterminismError("cannot remove " + native(directory) + ": " + ec.message());
        }
        note("players");
    }
    if (stale.loaders) {
        fs::path loaders = root / "prebuilt" / "loaders" / kArm64Key;
        fs::remove_all(loaders, ec);
        if (ec) throw DcDeterminismError("cannot remove " + native(loaders) + ": " + ec.message());
        note("loaders");
    }
    if (stale.any()) {
        log("the stale linux-arm64 data (" + kinds + ") was removed with its manifest entries: it is rebuilt on " + kArm64Rebuild);
    }
}

// --- the shipped loaders ----------------------------------------------------------------------------------
//
// prebuilt/loaders/<platform>/<compiler>/ and prebuilt/loaders/<platform>/manifest.json (schemas
// dc-prebuilt-windows-amd64-v1, dc-prebuilt-linux-amd64-v1 and dc-prebuilt-linux-arm64-v1). The Windows loaders come from
// the localbuild recipe (`build_loader`, MSVC and MSYS2 gcc/clang, with the MinGW runtime DLLs as sidecars); the Linux
// loaders are built in the Docker image with the same cmake options as that recipe on a Linux host (linux-arm64: the
// four ids gcc, clang, gcc-nofma, clang-nofma, with the fused multiply-add count of each library).

const char* const kLoadersRel = "prebuilt/loaders";
const char* const kWindowsPlatform = "windows-amd64";

struct LoaderBuild {
    std::string platform;
    std::string compiler;  // msvc, gcc, clang (linux-arm64: also gcc-nofma, clang-nofma)
    std::string filename;
    fs::path library;
    std::vector<std::pair<std::string, fs::path>> sidecars;
    Json build;
};

struct LoaderSet {
    std::vector<LoaderBuild> builds;
    std::string linux_platform;  // linux-amd64 or linux-arm64: the platform of the Linux loaders
    std::string linux_image;
    std::string linux_image_id;
    Json windows_toolchain;  // `toolchain` of the Windows loader manifest
};

fs::path loader_directory(const fs::path& root, const std::string& platform) {
    return root / path_from_utf8(kLoadersRel) / platform;
}

void build_windows_loaders(const fs::path& root, const fs::path& scratch, LoaderSet& set) {
    for (const char* id : {"msvc", "gcc", "clang"}) {
        std::string compiler = id;
        BuiltLoader built = build_loader(compiler, root, false);  // never from the build cache
        LoaderBuild build;
        build.platform = kWindowsPlatform;
        build.compiler = compiler;
        build.filename = path_to_utf8(built.path.filename());
        fs::path staged = scratch / "loaders" / kWindowsPlatform / compiler;
        std::error_code ec;
        fs::create_directories(staged, ec);
        build.library = staged / built.path.filename();
        copy_replacing(built.path, build.library);
        Json output = json_get(built.receipt, "output");
        Json sidecars = json_get(output, "sidecars");
        if (sidecars.is_object()) {
            for (auto it = sidecars.begin(); it != sidecars.end(); ++it) {
                fs::path name = path_from_utf8(it.key()).filename();
                fs::path source = built.path.parent_path() / name;
                fs::path target = staged / name;
                copy_replacing(source, target);
                build.sidecars.emplace_back(path_to_utf8(name), target);
            }
        }
        Json compiler_info = json_get(built.receipt, "compiler");
        Json flags = json_get(built.receipt, "flags");
        build.build = Json::object();
        build.build["executable"] = json_get(compiler_info, "executable");
        build.build["version"] = json_get(compiler_info, "version");
        build.build["target"] = json_get(compiler_info, "target");
        build.build["cmake"] = json_get(flags, "cmake");
        build.build["generator"] = json_get(flags, "generator");
        build.build["configure"] = json_get(flags, "configure");
        build.build["build"] = json_get(flags, "build");
        build.build["receipt_identity"] = json_get(built.receipt, "identity");
        set.builds.push_back(std::move(build));
    }
}

// `ldd` output without the address each library was mapped at ("(0x00007f...)"): it changes from run to run (address
// space randomisation), and the manifest records what the libraries are, not where one run put them.
std::string without_load_addresses(const std::string& ldd_output) {
    std::string out;
    for (const std::string& line : text_lines(ldd_output)) {
        std::size_t at = line.rfind(" (0x");
        bool address = at != std::string::npos && line.back() == ')' && line.find_first_not_of("0123456789abcdefABCDEF", at + 4) == line.size() - 1;
        out += (address ? line.substr(0, at) : line) + "\n";
    }
    return out;
}

void build_linux_loaders(const fs::path& root, const fs::path& scratch, const std::string& image,
                         const std::string& image_id, const LinuxTarget& target, LoaderSet& set) {
    fs::path box2d = prepare_box2d(root, scratch / "loader-inputs");
    set.linux_platform = target.platform;
    set.linux_image = image;
    set.linux_image_id = image_id;
    for (const std::string& compiler : target.compilers()) {
        fs::path out = scratch / "linux-loaders";
        std::error_code ec;
        fs::create_directories(out, ec);
        make_world_writable(out);
        std::vector<Mount> mounts = {{root, "/src:ro"}, {out, "/out"}, {box2d, "/deps/box2d-patched:ro"}};
        for (const char* dependency : {"fpm", "nlohmann-json", "xxhash"}) {
            mounts.push_back({root / "src" / "vendor" / dependency, std::string("/deps/") + dependency + ":ro"});
        }
        log("building the " + target.platform + " " + compiler + " loader");
        run_script(scratch, "docker linux " + compiler + " loader", image, mounts, {{"COMP", compiler}}, "linux-loader.sh",
                   linux_loader_script(target.arm64()), true, true);
        LoaderBuild build;
        build.platform = target.platform;
        build.compiler = compiler;
        build.filename = "libdigitalcurling_plugin_loader.so";
        build.library = out / compiler / build.filename;
        if (!fs::is_regular_file(build.library, ec)) throw DcDeterminismError("the linux loader was not created: " + native(build.library));
        build.build = Json::object();
        build.build["compiler_version"] = py_strip(read_file(out / compiler / "compiler.txt"));
        build.build["target"] = py_strip(read_file(out / compiler / "target.txt"));
        build.build["cmake"] = py_strip(read_file(out / compiler / "cmake.txt"));
        build.build["generator"] = "Unix Makefiles";
        build.build["build_command"] = "cmake --build <build-dir> --config Release --target digitalcurling_plugin_loader --parallel <jobs>";
        if (target.arm64()) {
            // The fused multiply-add instructions of the library (objdump in the container): a nofma id must have none.
            bool nofma = compiler.size() > 6 && compiler.compare(compiler.size() - 6, 6, "-nofma") == 0;
            std::string counted = py_strip(read_file(out / compiler / "fma.txt"));
            bool numeric = !counted.empty() &&
                           std::all_of(counted.begin(), counted.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
            if (!numeric) {
                throw DcDeterminismError("the fused multiply-add count of the " + compiler + " loader is not a number: '" + counted + "'");
            }
            std::int64_t fused = std::stoll(counted);
            if (nofma && fused != 0) {
                throw DcDeterminismError("the " + compiler + " loader contains " + counted +
                                         " fused multiply-add instructions (objdump); a nofma loader must contain none");
            }
            if (nofma) build.build["flags_extra"] = Json::parse(R"(["-ffp-contract=off"])");
            build.build["fused_multiply_add_instructions"] = fused;
            log(target.platform + " " + compiler + " loader: " + counted + " fused multiply-add instructions");
        }
        build.build["runtime_dependencies"] = without_load_addresses(read_file(out / compiler / "ldd.txt"));
        set.builds.push_back(std::move(build));
    }
}

// `source` and `dependencies` of the loader manifests: the vendored simulator and its dependencies, verified.
std::pair<Json, Json> loader_identity(const fs::path& root) {
    Json sources = loader_sources(root);
    Json deps_manifest = read_json(root / path_from_utf8(kVendorManifestRel));
    Json dependencies = Json::object();
    for (const char* name : kLoaderDependencies) {
        dependencies[name] = json_get(json_get(json_get(deps_manifest, "dependencies"), name), "upstream_archive_sha256");
    }
    return {json_get(sources, "simulator"), dependencies};
}
// The manifest of one platform.
Json loader_manifest(const LoaderSet& set, const std::string& platform, const Json& source, const Json& dependencies) {
    Json manifest = Json::object();
    std::string schema = "dc-prebuilt-" + platform + "-v1";  // windows-amd64, linux-amd64, linux-arm64
    manifest["schema"] = schema;
    manifest["image_id"] = platform == kWindowsPlatform ? Json("native-windows-amd64") : Json(set.linux_image_id);
    manifest["source"] = source;
    manifest["dependencies"] = dependencies;
    if (platform == kWindowsPlatform && set.windows_toolchain.is_object()) manifest["toolchain"] = set.windows_toolchain;
    Json libraries = Json::object();
    for (const LoaderBuild& build : set.builds) {
        if (build.platform != platform) continue;
        Json entry = Json::object();
        entry["filename"] = build.filename;
        entry["sha256"] = sha256_file(build.library);
        Json sidecars = Json::object();
        for (const auto& item : build.sidecars) sidecars[item.first] = sha256_file(item.second);
        entry["sidecars"] = sidecars;
        entry["build"] = build.build;
        libraries[build.compiler] = entry;
    }
    manifest["libraries"] = libraries;
    return manifest;
}

// The platforms of the built loaders.
std::vector<std::string> loader_platforms(const LoaderSet& set) {
    std::vector<std::string> platforms;
    for (const LoaderBuild& build : set.builds) {
        if (std::find(platforms.begin(), platforms.end(), build.platform) == platforms.end()) platforms.push_back(build.platform);
    }
    return platforms;
}

// Installs the built loaders and their manifests (the stale linux-arm64 loaders of a Windows host are removed last).
void install_loaders(const fs::path& root, const LoaderSet& set, const std::pair<Json, Json>& identity) {
    std::error_code ec;
    for (const std::string& platform : loader_platforms(set)) {
        fs::path directory = loader_directory(root, platform);
        fs::path manifest_path = directory / "manifest.json";
        // A compiler directory that is not built any more goes with the rebuild (the platform's directory is rebuilt whole).
        for (const LoaderBuild& build : set.builds) {
            if (build.platform != platform) continue;
            fs::path target_dir = directory / build.compiler;
            fs::remove_all(target_dir, ec);
            fs::create_directories(target_dir, ec);
            copy_replacing(build.library, target_dir / path_from_utf8(build.filename));
            for (const auto& item : build.sidecars) copy_replacing(item.second, target_dir / path_from_utf8(item.first));
        }
        write_replacing(manifest_path, manifest_text(loader_manifest(set, platform, identity.first, identity.second)));
        log("installed the " + platform + " loaders and manifest");
    }
}

}  // namespace

PrebuildHooks& prebuild_hooks() {
    static PrebuildHooks hooks;
    return hooks;
}

// --- source identity -----------------------------------------------------------------------------

SourceIdentity source_identity(const fs::path& root) {
    // Verifies the vendored player library and dependency trees against their recorded hashes.
    Json sources = player_sources(root);
    SourceIdentity identity;
    identity.worker_sha256 = sha256_lf_file(root / path_from_utf8(kWorkerRel));
    identity.player_worker_sha256 = sha256_lf_file(root / path_from_utf8(kPlayerWorkerRel));
    identity.player_library = json_get(sources, "player_library");
    identity.dependencies = Json::object();
    Json vendored = json_get(sources, "dependencies");
    for (const auto& item : kPlayerDependencies) {
        Json entry = json_get(vendored, item.second);
        Json out = Json::object();
        out["commit"] = json_get(entry, "pin");
        out["git_tree"] = json_get(entry, "git_tree");
        out["include_tree_sha256_lf"] = tree_sha256_lf(root / "src" / "vendor" / item.second / "include");
        identity.dependencies[item.first] = out;
    }
    return identity;
}

Json players_source_json(const SourceIdentity& identity) {
    Json worker = Json::object();
    worker["path"] = kPlayerWorkerRel;
    worker["sha256"] = identity.player_worker_sha256;
    Json source = Json::object();
    source["worker"] = worker;
    source["player_library"] = identity.player_library;
    source["dependencies"] = identity.dependencies;
    return source;
}

std::optional<std::string> identity_mismatch(const Json& manifest, const SourceIdentity& identity, bool players) {
    std::optional<std::string> mismatch = worker_mismatch(manifest, identity);
    if (mismatch || !players) return mismatch;
    return players_mismatch(manifest, identity);
}

std::optional<std::string> worker_mismatch(const Json& manifest, const SourceIdentity& identity) {
    std::string recorded = member_text(manifest, {"source", "sha256"});
    if (identity.worker_sha256 != recorded) {
        return "worker.cpp changed (sha256 " + identity.worker_sha256 + ", manifest " + recorded + ").";
    }
    return std::nullopt;
}

std::optional<std::string> players_mismatch(const Json& manifest, const SourceIdentity& identity) {
    if (!json_get(manifest, "players").is_object()) return std::string("the manifest has no players section.");
    Json source = json_get(json_get(manifest, "players"), "source");
    if (identity.player_worker_sha256 != member_text(source, {"worker", "sha256"})) {
        return "src/worker/player.cpp changed (sha256 " + identity.player_worker_sha256 + ").";
    }
    for (const char* key : {"revision", "git_tree", "tree_sha256_lf"}) {
        std::string now = member_text(identity.player_library, {key});
        if (now != member_text(source, {"player_library", key})) {
            return "vendored player library " + std::string(key) + " " + now + " differs from the manifest.";
        }
    }
    for (const auto& item : kPlayerDependencies) {
        Json now = json_get(identity.dependencies, item.first);
        Json then = json_get(json_get(source, "dependencies"), item.first);
        std::string key = item.first;
        if (member_text(now, {"commit"}) != member_text(then, {"commit"})) {
            return key + " HEAD differs from the pin " + member_text(then, {"commit"}) + ".";
        }
        if (member_text(now, {"git_tree"}) != member_text(then, {"git_tree"})) {
            return key + " Git tree id differs from the manifest.";
        }
        if (member_text(now, {"include_tree_sha256_lf"}) != member_text(then, {"include_tree_sha256_lf"})) {
            return key + " include tree " + member_text(now, {"include_tree_sha256_lf"}) + " differs from the manifest.";
        }
    }
    return std::nullopt;
}

namespace {

// The first difference of the loader identity `ids` (what the sources give) and the installed loader manifests.
std::optional<std::string> loader_manifests_mismatch(const fs::path& root, const std::pair<Json, Json>& ids) {
    for (const char* platform : kLoaderPlatforms) {
        std::string relative = std::string(kLoadersRel) + "/" + platform + "/manifest.json";
        std::error_code ec;
        if (!fs::is_regular_file(root / path_from_utf8(relative), ec)) continue;
        Json manifest = read_json(root / path_from_utf8(relative));
        if (canonical_json(json_get(manifest, "source")) != canonical_json(ids.first)) {
            return "the loader source differs from " + relative + " (the sources: " + canonical_json(ids.first) + ").";
        }
        if (canonical_json(json_get(manifest, "dependencies")) != canonical_json(ids.second)) {
            return "the loader dependencies differ from " + relative + " (the sources: " + canonical_json(ids.second) + ").";
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::string> loaders_mismatch(const fs::path& root) { return loader_manifests_mismatch(root, loader_identity(root)); }

// --- flags and scripts -----------------------------------------------------------------------------

std::vector<std::string> windows_worker_flags() { return {"-std=c++17", "-O2", "-static", "-s", "-lshell32"}; }

std::vector<std::string> linux_worker_flags() {
    return {"-std=c++17", "-O2", "-static-libstdc++", "-static-libgcc", "-s", "-ldl"};
}

std::vector<std::string> linux_arm64_worker_flags() {
    return {"-std=c++17", "-O2", "-ffp-contract=off", "-static-libstdc++", "-static-libgcc", "-s", "-ldl"};
}

std::vector<std::string> prebuilt_player_flags(const std::string& target, const std::string& player) {
    if (target == "windows-msvc") return player_flags("msvc", player);
    if (target == "wasm") return player_flags("wasm", player);
    std::vector<std::string> flags = {"-std=c++17", "-O3", "-DNDEBUG", "-Wno-deprecated-declarations"};
    if (player == "nd2") flags.push_back("-DPLAYER_ND2");
    if (target == "windows-gcc" || target == "windows-clang") {
        flags.push_back("-static");
    } else if (target == "linux-gcc" || target == "linux-clang" || target == "linux-gcc-nofma" || target == "linux-clang-nofma") {
        flags.push_back("-static-libstdc++");
        flags.push_back("-static-libgcc");
        if (target.size() > 6 && target.compare(target.size() - 6, 6, "-nofma") == 0) flags.push_back("-ffp-contract=off");
    } else {
        throw DcDeterminismError("unknown player build target: " + target);
    }
    return flags;
}

namespace {

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

}  // namespace

// The scripts start with `rm -rf` of every fixed work directory they use outside their mounts, so that a second run in
// the same container (the in-container executor) or a stale directory never leaks into a build.
std::string linux_worker_script(bool arm64) {
    std::string script = R"BASH(set -euo pipefail
rm -rf /tmp/dc_worker
g++ --version | head -n 1 > /out/compiler.txt
g++ -std=c++17 -O2 %FP%-static-libstdc++ -static-libgcc -s "-DDC_WORKER_SOURCE_SHA256=\"$SOURCE_SHA\"" /src/src/worker/worker.cpp -ldl -o /out/dc_worker
cp /out/dc_worker /tmp/dc_worker
chmod +x /tmp/dc_worker
echo "self-test: $(/tmp/dc_worker --self-test)"
echo "version: $(/tmp/dc_worker --version)"
)BASH";
    return replace_all(script, "%FP%", arm64 ? "-ffp-contract=off " : "");
}

// The same cmake options as `loader_configure_args` (localbuild) on a Linux host. linux-arm64: the ids gcc-nofma and
// clang-nofma add -ffp-contract=off to the C and C++ flags, and every library is counted for fused multiply-add
// instructions (objdump; $out/fma.txt).
std::string linux_loader_script(bool arm64) {
    std::string head = R"BASH(set -euo pipefail
%OBJDUMP%cxx=g++
cc=gcc
%COMPILER%%FLAGS%b=/tmp/loader-$COMP
rm -rf "$b"
out=/out/$COMP
mkdir -p "$out"
"$cxx" --version | head -n 1 > "$out/compiler.txt"
"$cxx" -dumpmachine > "$out/target.txt"
cmake --version | head -n 1 > "$out/cmake.txt"
cmake -S /src/src/vendor/digitalcurling -B "$b" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$(command -v $cxx)" -DCMAKE_C_COMPILER="$(command -v $cc)" -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON %CXXFLAGS%\
    -DBOX2D_BUILD_TESTBED=OFF -DBOX2D_BUILD_UNIT_TESTS=OFF -DDIGITALCURLING_BUILD_PLAYERS=OFF \
    -DDIGITALCURLING_BUILD_SIMULATORS=ON -DDIGITALCURLING_BUILD_SIMULATOR_FCV1=ON \
    -DDIGITALCURLING_BUILD_SIMULATOR_FCV1PLUS=ON -DDIGITALCURLING_BUILD_PLUGIN_LOADER=ON \
    -DDIGITALCURLING_PLUGIN_LOADER_SHARED=ON -DDIGITALCURLING_BUNDLE_PLUGINS=ON \
    -DDIGITALCURLING_BUILD_TEST=OFF -DDIGITALCURLING_BUILD_DOCS=OFF \
    -DFETCHCONTENT_SOURCE_DIR_FPM=/deps/fpm -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=/deps/nlohmann-json \
    -DFETCHCONTENT_SOURCE_DIR_BOX2D=/deps/box2d-patched \
    -DFETCHCONTENT_SOURCE_DIR_XXHASH=/deps/xxhash -DFETCHCONTENT_FULLY_DISCONNECTED=ON
cmake --build "$b" --config Release --target digitalcurling_plugin_loader --parallel "$(nproc)"
cp "$b/src/plugin-loader/libdigitalcurling_plugin_loader.so" "$out/"
ldd "$out/libdigitalcurling_plugin_loader.so" > "$out/ldd.txt" || true
%COUNT%)BASH";
    if (!arm64) {
        head = replace_all(head, "%OBJDUMP%", "");
        head = replace_all(head, "%COMPILER%", "if [ \"$COMP\" = clang ]; then cxx=clang++; cc=clang; fi\n");
        head = replace_all(head, "%FLAGS%", "");
        head = replace_all(head, "%CXXFLAGS%", "\"-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations\" ");
        head = replace_all(head, "%COUNT%", "");
        return head;
    }
    head = replace_all(head, "%OBJDUMP%",
                       "command -v objdump >/dev/null 2>&1 || { echo \"objdump was not found in the image (binutils): the fused "
                       "multiply-add gate of the linux-arm64 loaders needs it\" >&2; exit 1; }\n");
    head = replace_all(head, "%COMPILER%", "case \"$COMP\" in clang*) cxx=clang++; cc=clang ;; esac\n");
    head = replace_all(head, "%FLAGS%", R"BASH(extra=""
case "$COMP" in *-nofma) extra="-ffp-contract=off" ;; esac
cxxflags="-Wno-deprecated-declarations"
c_args=()
if [ -n "$extra" ]; then
    cxxflags="$cxxflags $extra"
    c_args=("-DCMAKE_C_FLAGS=$extra")
fi
)BASH");
    head = replace_all(head, "%CXXFLAGS%", "\"-DCMAKE_CXX_FLAGS=$cxxflags\" ${c_args[@]+\"${c_args[@]}\"} ");
    head = replace_all(head, "%COUNT%", R"BASH(objdump -d --no-show-raw-insn "$out/libdigitalcurling_plugin_loader.so" | awk -F'\t' '
    BEGIN {
        n = split("fmadd fmsub fnmadd fnmsub fmla fmls fmad fmsb fnmla fnmls fnmad fnmsb", names, " ")
        for (i = 1; i <= n; i++) want[names[i]] = 1
    }
    NF >= 2 { m = $2; sub(/[ \t].*$/, "", m); if (m in want) total++ }
    END { print total + 0 }' > "$out/fma.txt"
)BASH");
    return head;
}

std::string linux_players_script(bool arm64) {
    std::string script = R"BASH(set -euo pipefail
P=/player/src/player/src
mkdir -p /out/smoke
g++ --version | head -n 1 > /out/gcc.txt
clang++ --version | head -n 1 > /out/clang.txt
for comp in %COMPILERS%; do
    cxx=g++
    case "$comp" in clang*) cxx=clang++ ;; esac
%EXTRA%    for player in normal_dist nd2; do
        dir=/out/%PLATFORM%-$comp/$player
        mkdir -p "$dir"
        def=""
        if [ "$player" = nd2 ]; then def="-DPLAYER_ND2"; fi
        $cxx -std=c++17 -O3 -DNDEBUG -Wno-deprecated-declarations $def \
            -I /player/src/core/include -I /player/src/plugin-api/include \
            -I $P/$player -I $P/nd2 -I /deps/json/include -I /deps/fpm/include \
            /src/src/worker/player.cpp $P/$player/player_$player.cpp \
            $P/$player/player_${player}_factory.cpp $P/$player/player_${player}_storage.cpp \
            $P/nd2/xoshiro256plusplus.cpp \
            -static-libstdc++ -static-libgcc%FLAG% -o "$dir/worker"
        "$dir/worker" /out/smoke/%PLATFORM%-$comp-$player.bin 640 > /out/smoke/%PLATFORM%-$comp-$player.meta.json
    done
done
)BASH";
    script = replace_all(script, "%COMPILERS%", arm64 ? "gcc clang gcc-nofma clang-nofma" : "gcc clang");
    script = replace_all(script, "%PLATFORM%", arm64 ? "linux-arm64" : "linux-amd64");
    script = replace_all(script, "%EXTRA%", arm64 ? "    extra=\"\"\n    case \"$comp\" in *-nofma) extra=\"-ffp-contract=off\" ;; esac\n" : "");
    script = replace_all(script, "%FLAG%", arm64 ? " $extra" : "");
    return script;
}

std::string wasm_players_script() {
    return R"BASH(set -euo pipefail
P=/player/src/player/src
mkdir -p /out/smoke
em++ --version | head -n 1 > /out/em.txt
node --version > /out/node.txt
for player in normal_dist nd2; do
    dir=/out/wasm/$player
    mkdir -p "$dir"
    def=""
    if [ "$player" = nd2 ]; then def="-DPLAYER_ND2"; fi
    em++ -std=c++17 -O3 -DNDEBUG -Wno-deprecated-declarations \
        -fexceptions -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node $def \
        -I /player/src/core/include -I /player/src/plugin-api/include \
        -I $P/$player -I $P/nd2 -I /deps/json/include -I /deps/fpm/include \
        /src/src/worker/player.cpp $P/$player/player_$player.cpp \
        $P/$player/player_${player}_factory.cpp $P/$player/player_${player}_storage.cpp \
        $P/nd2/xoshiro256plusplus.cpp \
        -o "$dir/worker.js"
    node "$dir/worker.js" /out/smoke/wasm-container-$player.bin 640 > /out/smoke/wasm-container-$player.meta.json
done
)BASH";
}

// --- manifest -----------------------------------------------------------------------------

Json worker_source_json(const std::string& sha256) {
    Json source = Json::object();
    source["path"] = kWorkerRel;
    source["sha256"] = sha256;
    return source;
}

Json worker_binary_json(const std::string& path, const std::string& sha256, const std::string& compiler,
                        const std::vector<std::string>& flags, const std::string& image, const std::string& image_id) {
    Json entry = Json::object();
    entry["path"] = path;
    entry["sha256"] = sha256;
    entry["compiler"] = compiler;
    entry["flags"] = json_array_of(flags);
    if (!image.empty()) {
        entry["image"] = image;
        entry["image_id"] = image_id;
    }
    return entry;
}

Json player_entry_json(const std::string& path, const std::string& sha256, const std::string& platform,
                       const std::string& compiler_id, const std::string& compiler, const std::string& player,
                       const std::vector<std::string>& flags, const std::vector<std::pair<std::string, Json>>& extra) {
    Json entry = Json::object();
    entry["path"] = path;
    entry["sha256"] = sha256;
    entry["platform"] = platform;
    entry["compiler_id"] = compiler_id;
    entry["compiler"] = compiler;
    entry["player"] = player;
    entry["flags"] = json_array_of(flags);
    for (const auto& item : extra) entry[item.first] = item.second;
    return entry;
}

namespace {

// The rank of a key of `binaries` or `players.binaries` by platform: windows-amd64, linux-amd64, linux-arm64, wasm, others.
int platform_rank(const std::string& key) {
    if (starts_with(key, "windows-amd64")) return 0;
    if (starts_with(key, "linux-amd64")) return 1;
    if (starts_with(key, "linux-arm64")) return 2;
    if (starts_with(key, "wasm")) return 3;
    return 4;
}

// The members of `object` ordered by platform (the order inside a platform is kept).
Json ordered_by_platform(const Json& object) {
    std::vector<std::pair<std::string, Json>> items;
    for (auto it = object.begin(); it != object.end(); ++it) items.emplace_back(it.key(), it.value());
    std::stable_sort(items.begin(), items.end(), [](const std::pair<std::string, Json>& a, const std::pair<std::string, Json>& b) {
        return platform_rank(a.first) < platform_rank(b.first);
    });
    Json out = Json::object();
    for (const auto& item : items) out[item.first] = item.second;
    return out;
}

}  // namespace

Json merge_manifest(const Json& existing, const ManifestUpdate& update) {
    Json out = existing.is_object() ? existing : Json::object();
    std::string schema = member_text(existing, {"schema"});
    out["schema"] = schema.empty() ? std::string(kPrebuiltManifestSchema) : schema;
    if (!update.source.is_null()) out["source"] = update.source;

    Json binaries = json_get(existing, "binaries").is_object() ? json_get(existing, "binaries") : Json::object();
    for (const std::string& key : update.removed_binaries) binaries.erase(key);
    for (auto it = update.binaries.begin(); it != update.binaries.end(); ++it) binaries[it.key()] = it.value();
    out["binaries"] = ordered_by_platform(binaries);

    Json existing_players = json_get(existing, "players");
    if (!update.players_source.is_null() || !update.player_entries.empty() || !update.replaced_player_prefixes.empty()) {
        Json players = existing_players.is_object() ? existing_players : Json::object();
        if (!update.players_source.is_null()) players["source"] = update.players_source;
        Json entries = json_get(players, "binaries").is_object() ? json_get(players, "binaries") : Json::object();
        Json kept = Json::object();
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (!has_prefix(it.key(), update.replaced_player_prefixes)) kept[it.key()] = it.value();
        }
        for (auto it = update.player_entries.begin(); it != update.player_entries.end(); ++it) kept[it.key()] = it.value();
        players["binaries"] = ordered_by_platform(kept);
        out["players"] = players;
    }

    Json toolchain = merge_toolchain(json_get(existing, "toolchain"), update.toolchain);
    for (const std::string& key : update.removed_toolchain) toolchain.erase(key);
    if (toolchain.is_object()) toolchain = ordered_by_platform(toolchain);
    if (existing.contains("toolchain") || !toolchain.empty()) out = with_toolchain(out, toolchain);
    return out;
}

std::string manifest_text(const Json& manifest) { return json_dumps(manifest, DumpOptions::pretty(2)) + "\n"; }

// --- smoke outputs -----------------------------------------------------------------------------

SmokeReport check_smoke(const std::vector<std::pair<std::string, std::vector<SmokeRun>>>& players) {
    SmokeReport report;
    for (const auto& player : players) {
        std::vector<std::pair<std::string, PdetRecord>> decoded;
        for (const SmokeRun& run : player.second) {
            try {
                PdetRecord record = decode_pdet(read_file(run.path));
                if (record.restore_failures != 0) {
                    report.lines.push_back("FAIL " + player.first + " " + run.label +
                                           ": restore_failures=" + std::to_string(record.restore_failures));
                    report.failed = true;
                }
                report.lines.push_back("decoded " + player.first + " " + run.label + ": samples=" +
                                       std::to_string(record.samples) + " sha256=" + record.sha256);
                decoded.emplace_back(run.label, std::move(record));
            } catch (const PdetError& error) {
                report.lines.push_back("FAIL " + player.first + " " + run.label + ": invalid player output " +
                                       native(run.path) + ": " + error.what());
                report.failed = true;
            }
        }
        for (std::size_t i = 1; i < decoded.size(); ++i) {
            const auto& base = decoded.front();
            std::string head = "compare " + player.first + " " + base.first + " vs " + decoded[i].first + ": ";
            try {
                Json result = compare_pdet(base.second, decoded[i].second);
                bool same_bytes = base.second.sha256 == decoded[i].second.sha256;
                std::int64_t different = result["different_samples"].get<std::int64_t>();
                report.lines.push_back(head + "different_samples=" + std::to_string(different) +
                                       " byte_identical=" + (same_bytes ? "true" : "false"));
            } catch (const PdetError& error) {
                report.lines.push_back("FAIL " + head + "cannot compare player outputs: " + error.what());
                report.failed = true;
            }
        }
    }
    return report;
}

// --- the WebAssembly dc_worker -----------------------------------------------------------------------------

WasmSources wasm_sources(const fs::path& root, bool require_current_manifest,
                         const std::optional<std::pair<Json, Json>>& loader) {
    WasmSources sources;
    sources.worker_sha256 = sha256_lf_file(root / path_from_utf8(kWorkerRel));
    if (require_current_manifest) {
        Json native_manifest = read_manifest(root);
        std::string recorded = member_text(native_manifest, {"source", "sha256"});
        std::string recorded_path = member_text(native_manifest, {"source", "path"});
        if (recorded != sources.worker_sha256 || recorded_path != kWorkerRel) {
            throw DcDeterminismError(std::string("stale prebuilt/manifest.json: it records ") + recorded + " for " + recorded_path + ", " +
                             kWorkerRel + " is " + sources.worker_sha256 + " (run `dc_determinism prebuild` first)");
        }
    }

    // The loader manifests of the platforms must agree about the simulator source and the dependencies (linux-arm64
    // is optional; when present it must agree with the others). A full rebuild gives the identity of the loaders it
    // has just built instead: the installed manifests are neither read nor needed.
    std::vector<std::pair<std::string, Json>> loaders;
    if (loader) {
        Json identity = Json::object();
        identity["source"] = loader->first;
        identity["dependencies"] = loader->second;
        loaders.emplace_back("rebuilt", identity);
    } else {
        for (const char* platform : kLoaderPlatforms) {
            fs::path path = root / "prebuilt" / "loaders" / platform / "manifest.json";
            std::error_code ec;
            if (std::string(platform) == kArm64Key && !fs::is_regular_file(path, ec)) continue;
            loaders.emplace_back(platform, read_json(path));
        }
    }
    auto canonical = [](const Json& value) { return json_dumps(value, DumpOptions::compact(true)); };
    for (const char* member : {"source", "dependencies"}) {
        std::string first = canonical(json_get(loaders.front().second, member));
        for (std::size_t i = 1; i < loaders.size(); ++i) {
            if (canonical(json_get(loaders[i].second, member)) != first) {
                throw DcDeterminismError("the " + loaders.front().first + " and " + loaders[i].first + " loader manifests disagree about '" +
                                 member + "'");
            }
        }
    }
    const Json& loader_manifest = loaders.front().second;
    sources.loader_source = json_get(loader_manifest, "source");
    sources.loader_dependencies = json_get(loader_manifest, "dependencies");
    sources.simulator_tree = member_text(loader_manifest, {"source", "tree_sha256_lf"});

    log("verifying the vendored simulator tree ...");
    std::string actual = tree_sha256_lf(root / path_from_utf8(kSimulatorRel));
    if (actual != sources.simulator_tree) {
        throw DcDeterminismError(std::string(kSimulatorRel) + " does not match the loader manifests: tree sha256 is " + actual +
                         ", the manifests record " + sources.simulator_tree);
    }
    std::string deps_manifest_rel = kVendorManifestRel;
    Json deps_manifest = read_json(root / path_from_utf8(deps_manifest_rel));
    sources.verified_dependencies = Json::object();
    for (const char* dependency : kLoaderDependencies) {
        std::string name = dependency;
        Json entry = json_get(json_get(deps_manifest, "dependencies"), dependency);
        if (!entry.is_object()) throw DcDeterminismError(deps_manifest_rel + " has no entry for " + name);
        std::string archive = member_text(entry, {"upstream_archive_sha256"});
        std::string loader_archive = member_text(sources.loader_dependencies, {dependency});
        if (archive != loader_archive) {
            throw DcDeterminismError("dependency " + name + ": " + deps_manifest_rel + " records upstream archive " + archive +
                             ", the loader manifests " + loader_archive);
        }
        std::string tree = tree_sha256_lf(root / path_from_utf8(kDepsRel) / name);
        std::string recorded_tree = member_text(entry, {"tree_sha256_lf"});
        if (tree != recorded_tree) {
            throw DcDeterminismError("dependency " + name + ": " + kDepsRel + "/" + name + " tree sha256 is " + tree + ", " +
                             deps_manifest_rel + " records " + recorded_tree);
        }
        Json verified = Json::object();
        verified["pin"] = json_get(entry, "pin");
        verified["git_tree"] = json_get(entry, "git_tree");
        verified["upstream_archive_sha256"] = archive;
        verified["tree_sha256_lf"] = tree;
        sources.verified_dependencies[name] = verified;
    }
    log("vendored simulator " + sources.simulator_tree + " and " + std::to_string(sizeof(kLoaderDependencies) / sizeof(kLoaderDependencies[0])) +
        " dependency trees verified");
    return sources;
}

Json wasm_worker_manifest(const WasmSources& sources, const WasmWorkerFacts& facts) {
    const std::string bundle = kWasmBundleRel;
    const Json& info = facts.build_info;
    Json manifest = Json::object();
    manifest["schema"] = kWasmSchema;

    Json sidecars = Json::object();
    sidecars["dc_worker.wasm"] = facts.wasm_sha256;
    Json worker = Json::object();
    worker["path"] = bundle + "/dc_worker.js";
    worker["sha256"] = facts.js_sha256;
    worker["sidecars"] = sidecars;
    worker["runtime"] = "node";
    worker["node_min"] = "v18";
    manifest["worker"] = worker;

    Json glue = Json::object();
    glue["path"] = bundle + "/dc_worker.js";
    glue["sha256"] = facts.js_sha256;
    glue["note"] = "the Emscripten JavaScript runtime is dc_worker.js itself";
    manifest["glue"] = glue;

    Json source = Json::object();
    source["path"] = kWorkerRel;
    source["sha256"] = sources.worker_sha256;
    manifest["source"] = source;

    Json simulator = Json::object();
    simulator["source"] = sources.loader_source;
    simulator["dependencies"] = sources.loader_dependencies;
    manifest["simulator"] = simulator;

    Json simulator_tree = Json::object();
    simulator_tree["path"] = kSimulatorRel;
    simulator_tree["tree_sha256_lf"] = sources.simulator_tree;
    Json verified_trees = Json::object();
    verified_trees["simulator"] = simulator_tree;
    verified_trees["dependencies"] = sources.verified_dependencies;
    manifest["verified_trees"] = verified_trees;

    Json smoke_input = Json::object();
    smoke_input["path"] = kWasmSmokeInputRel;
    smoke_input["sha256"] = json_get(info, "smoke_input_sha256");
    Json build = Json::object();
    build["override_sha256"] = json_get(info, "override_sha256");
    build["shim_sha256"] = json_get(info, "shim_sha256");
    build["smoke_input"] = smoke_input;
    build["script"] = kWasmBuildScriptRel;
    manifest["build"] = build;

    Json toolchain_info = json_get(info, "toolchain");
    Json toolchain = Json::object();
    toolchain["image"] = facts.image;
    toolchain["image_id"] = facts.image_id;
    toolchain["repo_digests"] = json_array_of(facts.repo_digests);
    toolchain["emxx"] = json_get(toolchain_info, "emxx");
    toolchain["emxx_version_text"] = json_get(toolchain_info, "emxx_version_text");
    toolchain["cmake"] = json_get(toolchain_info, "cmake");
    toolchain["node"] = json_get(toolchain_info, "node");
    manifest["toolchain"] = toolchain;

    Json flags_info = json_get(info, "flags");
    Json flags = Json::object();
    flags["configure"] = json_get(flags_info, "configure");
    flags["link"] = json_get(flags_info, "link");
    flags["cmake_cache"] = json_get(flags_info, "cmake_cache");
    flags["fp_contract"] = json_get(flags_info, "fp_contract");
    manifest["flags"] = flags;

    Json smoke_info = json_get(info, "smoke");
    Json smoke = Json::object();
    smoke["version"] = json_get(smoke_info, "version");
    smoke["self_test"] = json_get(smoke_info, "self_test");
    smoke["seconds_per_frame"] = facts.host_seconds_per_frame;
    smoke["shots"] = facts.host_shots;
    smoke["container_node"] = json_get(toolchain_info, "node");
    smoke["host_node"] = facts.host_node;
    manifest["smoke"] = smoke;
    return manifest;
}

void check_prebuild_options(const PrebuildOptions& options) {
    if (!options.tool_fragments.empty()) {
        if (options.worker_only || options.wasm_worker_only || !options.image.empty() || options.emsdk_image_explicit ||
            !options.image_id.empty() || !options.build_user.empty()) {
            throw DcDeterminismError("--tool-fragment builds nothing: it accepts no other option than --check");
        }
        return;
    }
    if (!options.wasm_worker_only) return;
    if (options.worker_only) throw DcDeterminismError("--wasm-worker-only and --worker-only exclude each other");
    if (!options.image.empty()) {
        throw DcDeterminismError("--image does not apply to --wasm-worker-only (the Emscripten image is --emsdk-image)");
    }
}

namespace {

// Prints what is wrong with the toolchain and says that nothing was built (exit code 2).
int toolchain_refusal(const std::vector<std::string>& problems) {
    for (const std::string& problem : problems) report_line(problem);
    report_line("dc_determinism prebuild --check: the toolchain is not the recorded one, so nothing was rebuilt (exit code 2)");
    return 2;
}

// The WebAssembly bundle against the installed one: files, then the manifest without the host node version (it
// only reports the node that ran the smoke test, which does not enter the bytes).
void check_wasm_bundle(CheckReport& report, const fs::path& root, const WasmWorkerBuild& build) {
    fs::path bundle = root / path_from_utf8(kWasmBundleRel);
    check_artifact(report, "wasm dc_worker",
                   {{"dc_worker.js", bundle / "dc_worker.js", build.js}, {"dc_worker.wasm", bundle / "dc_worker.wasm", build.wasm}});
    std::string label = std::string(kWasmBundleRel) + "/manifest.json";
    Json installed = read_installed_json(root, label);
    Json rebuilt = build.manifest;
    if (installed.is_object() && installed["smoke"].is_object()) installed["smoke"].erase("host_node");
    if (rebuilt["smoke"].is_object()) rebuilt["smoke"].erase("host_node");
    check_manifest(report, label, installed, rebuilt);
}

void check_workers(CheckReport& report, const fs::path& root, const BuiltBinary* windows_worker, const BuiltBinary& linux_worker,
                   const LinuxTarget& target) {
    if (windows_worker != nullptr) {
        check_artifact(report, "dc_worker windows-amd64", {{"dc_worker.exe", root / path_from_utf8(kWindowsWorkerRel), windows_worker->path}});
    }
    check_artifact(report, "dc_worker " + target.platform, {{"dc_worker", root / path_from_utf8(target.worker_rel()), linux_worker.path}});
}

void check_players(CheckReport& report, const fs::path& root, const PlayerSet& set) {
    for (const PlayerBuild& build : set.builds) {
        fs::path installed = root / "prebuilt" / "players" / path_from_utf8(build.dir) / path_from_utf8(build.player) /
                             path_from_utf8(build.file);
        std::vector<CheckedFile> files = {{build.file, installed, build.built}};
        if (!build.built_wasm.empty()) files.push_back({"worker.wasm", installed.parent_path() / "worker.wasm", build.built_wasm});
        check_artifact(report, "player " + build.key, files);
    }
}

void check_loaders(CheckReport& report, const fs::path& root, const LoaderSet& set, const std::pair<Json, Json>& identity) {
    for (const LoaderBuild& build : set.builds) {
        fs::path directory = loader_directory(root, build.platform) / path_from_utf8(build.compiler);
        std::vector<CheckedFile> files = {{build.filename, directory / path_from_utf8(build.filename), build.library}};
        for (const auto& item : build.sidecars) files.push_back({item.first, directory / path_from_utf8(item.first), item.second});
        check_artifact(report, "loader " + build.platform + "/" + build.compiler, files);
    }
    for (const std::string& platform : loader_platforms(set)) {
        std::string relative = std::string(kLoadersRel) + "/" + platform + "/manifest.json";
        check_manifest(report, relative, read_installed_json(root, relative),
                       loader_manifest(set, platform, identity.first, identity.second));
    }
}

// The members of prebuilt/manifest.json a run rebuilds: `existing` is the installed manifest, the other three are what the
// run would write. Only the entries of the platforms the host builds are compared (`prefixes`: of `binaries` and of
// `players.binaries`); the carried platforms are not looked at.
void check_main_manifest(CheckReport& report, const Json& existing, const Json& source, const Json& binaries, const Json& players,
                         const std::vector<std::string>& binary_prefixes, const std::vector<std::string>& player_prefixes) {
    auto members = [&](const Json& s, const Json& b, const Json& p) {
        Json out = Json::object();
        out["source"] = s;
        out["binaries"] = entries_of(b, binary_prefixes);
        if (p.is_object()) {
            Json one = Json::object();
            one["source"] = json_get(p, "source");
            one["binaries"] = entries_of(json_get(p, "binaries"), player_prefixes);
            out["players"] = one;
        }
        return out;
    };
    Json installed = members(json_get(existing, "source"), json_get(existing, "binaries"),
                             players.is_object() ? json_get(existing, "players") : Json(nullptr));
    check_manifest(report, kManifestRel, installed, members(source, binaries, players));
}

// --- who owns the WebAssembly artifacts --------------------------------------------------------------------

struct WasmPlan {
    bool owned = false;      // this host builds (and checks) the WebAssembly dc_worker and players
    std::string emsdk_id;    // the id of the Emscripten image (owned)
};

// What the carried WebAssembly artifacts depend on, against what they were built from: the first difference, or nullopt
// (also when there is no installed bundle: nothing is carried).
std::optional<std::string> wasm_carry_problem(const fs::path& root, const Json& existing, const SourceIdentity& identity,
                                              const std::string& worker_sha, const std::pair<Json, Json>& loader_ids) {
    Json wasm = read_installed_json(root, std::string(kWasmBundleRel) + "/manifest.json");
    if (wasm.is_null()) return std::nullopt;
    std::string built_from = member_text(wasm, {"source", "sha256"});
    if (worker_sha != built_from) {
        return "worker.cpp changed (sha256 " + worker_sha + ", the wasm dc_worker was built from " + built_from + ")";
    }
    std::optional<std::string> mismatch = players_mismatch(existing, identity);
    if (mismatch) return "the player sources changed: " + *mismatch;
    Json simulator = json_get(wasm, "simulator");
    if (canonical_json(json_get(simulator, "source")) != canonical_json(loader_ids.first) ||
        canonical_json(json_get(simulator, "dependencies")) != canonical_json(loader_ids.second)) {
        return "the loader source or dependencies changed";
    }
    return std::nullopt;
}

// A host builds wasm only when its Emscripten image id equals the recorded one (or none is recorded). Otherwise it carries
// every wasm member unchanged, and only while the identities wasm depends on are the recorded ones: else exit code 2 naming
// the recorded image. An absent image counts as another id, unless --emsdk-image named it (then it is an error).
WasmPlan decide_wasm(const PrebuildOptions& options, const fs::path& root, const Json& existing, const SourceIdentity& identity,
                     const std::string& worker_sha, const std::pair<Json, Json>& loader_ids, const LinuxTarget& target) {
    WasmPlan plan;
    std::string recorded = member_text(existing, {"toolchain", "wasm", "image_id"});
    std::string recorded_image = member_text(existing, {"toolchain", "wasm", "image"});
    ToolOutput inspected = run_docker({"image", "inspect", "--format", "{{.Id}}", options.emsdk_image}, false);
    bool present = inspected.exit_code == 0 && !first_line(inspected.output).empty();
    std::string id = present ? first_line(inspected.output) : std::string();
    if (!present && options.emsdk_image_explicit) {
        throw DcDeterminismError("Docker image " + options.emsdk_image + " not found (--emsdk-image)");
    }
    // An image of another architecture cannot build here (nothing is emulated): named explicitly it is an error,
    // otherwise this host does not own wasm.
    bool foreign = false;
    if (present) {
        try {
            require_image_architecture(options.emsdk_image, target);
        } catch (const DcDeterminismError&) {
            if (options.emsdk_image_explicit) throw;
            foreign = true;
        }
    }
    if (present && !foreign && (recorded.empty() || id == recorded)) {
        plan.owned = true;
        plan.emsdk_id = id;
        log("docker image: " + options.emsdk_image + " (" + id + ")");
        return plan;
    }
    std::string state = !present ? options.emsdk_image + " (absent)" : foreign ? options.emsdk_image + " (" + id + ", not a " + target.arch + " image)" : id;
    log("wasm not rebuilt: emsdk image " + state + " is not the recorded " + (recorded.empty() ? std::string("<none>") : recorded) +
        "; the wasm members are carried");
    std::optional<std::string> problem = wasm_carry_problem(root, existing, identity, worker_sha, loader_ids);
    if (problem) {
        throw DcDeterminismError(
            "the WebAssembly dc_worker and players cannot be carried unchanged: " + *problem +
            ". Rebuild them with the recorded Emscripten image " + (recorded_image.empty() ? std::string("<not recorded>") : recorded_image) +
            " (" + (recorded.empty() ? std::string("no id recorded") : recorded) + "): pass it as --emsdk-image on a host that has it");
    }
    return plan;
}

}  // namespace

int run_wasm_worker_prebuild(const PrebuildOptions& options) {
    check_prebuild_options(options);
    if (!options.wasm_worker_only) throw DcDeterminismError("run_wasm_worker_prebuild needs wasm_worker_only");
    ExecutorScope executor_scope(options);
    fs::path root = repo_root_of(options);
    if (!prebuild_hooks().program && !find_on_path("node")) {
        throw DcDeterminismError("node is required on PATH for the host smoke test of the wasm worker");
    }
    // The check verifies against the loader identity of the sources (in memory), never the installed manifests.
    WasmSources sources = options.check ? wasm_sources(root, false, loader_identity(root)) : wasm_sources(root, true);
    log("dc_worker source sha256: " + sources.worker_sha256 + " (wasm worker only)");
    if (options.check) {
        Json current = Json::object();
        current[kWasmKey] = docker_toolchain(options.emsdk_image, image_id_of(options.emsdk_image));
        std::vector<std::string> problems = toolchain_problems(root, current, false, false, true);
        if (!problems.empty()) return toolchain_refusal(problems);
    }

    BuildScratch scratch(root);
    WasmWorkerBuild build = build_wasm_worker(scratch.root(), scratch.path(), options.emsdk_image, sources);
    if (options.check) {
        CheckReport report;
        check_wasm_bundle(report, root, build);
        report.print();
        return report.exit_code();
    }
    wasm_bundle_reproduced(root, build);
    install_wasm_worker(root, build);
    return 0;
}

// --- the commands -----------------------------------------------------------------------------

namespace {

// One run of the host that builds `target` (and, on a Windows host, windows-amd64 too): the flow of the header comment.
int run_host_prebuild(const PrebuildOptions& options, bool windows_host, const LinuxTarget& target) {
    check_prebuild_options(options);
    ExecutorScope executor_scope(options);
    fs::path root = repo_root_of(options);
    const bool players = !options.worker_only;
    std::string source_sha = sha256_lf_file(root / path_from_utf8(kWorkerRel));
    log("dc_worker source sha256: " + source_sha + (options.worker_only ? " (worker only)" : ""));

    // The recorded manifest. Only the Windows host may start without one (a full rebuild).
    Json existing = Json::object();
    std::error_code ec;
    if (fs::is_regular_file(root / path_from_utf8(kManifestRel), ec)) {
        existing = read_manifest(root);
        check_schema(existing);
    } else if (!windows_host) {
        refuse("prebuilt/manifest.json is missing.");
    } else if (options.worker_only || options.check) {
        throw DcDeterminismError("prebuilt/manifest.json is missing; run a full rebuild first.");
    }

    // The sources. Every other host than the Windows host refuses a source that differs from the manifests.
    SourceIdentity identity;
    std::pair<Json, Json> loader_ids;
    if (players) {
        try {
            identity = source_identity(root);
            loader_ids = loader_identity(root);
        } catch (const DcDeterminismError& error) {
            if (windows_host) throw;
            refuse(std::string("vendored source verification failed: ") + error.what());
        }
        log("player worker source sha256: " + identity.player_worker_sha256);
        log("player library " + member_text(identity.player_library, {"revision"}) + " verified (tree sha256 " +
            member_text(identity.player_library, {"tree_sha256_lf"}) + ")");
    } else {
        identity.worker_sha256 = source_sha;
    }
    if (!windows_host) {
        std::optional<std::string> mismatch = identity_mismatch(existing, identity, players);
        if (!mismatch && players) mismatch = loader_manifests_mismatch(root, loader_ids);
        if (mismatch) {
            if (options.check) {
                // The installed artifacts were not built from these sources: nothing to compare them with.
                CheckReport report;
                report.differs = true;
                report.add(std::string("manifest ") + kManifestRel + ": differs (" + *mismatch + ")");
                report.print();
                return report.exit_code();
            }
            refuse(*mismatch);
        }
    }
    // The carried linux-arm64 data that the new sources made stale (a Windows host; removed after the install).
    Arm64Stale stale;
    if (windows_host && !options.check) {
        stale = stale_arm64(root, existing, identity, players, players ? std::optional<std::pair<Json, Json>>(loader_ids) : std::nullopt);
        for (const std::string& reason : stale.reasons) log(reason);
    }

    // The Docker images (the image of this host's architecture; nothing is emulated) and the wasm decision.
    std::string image = select_image(options.image);
    require_image_architecture(image, target);
    std::string image_id = image_id_of(image);
    log("docker image: " + image + " (" + image_id + ")");
    WasmPlan wasm;
    if (players && !executor().container) wasm = decide_wasm(options, root, existing, identity, source_sha, loader_ids, target);
    if (wasm.owned && !program_available("node")) {
        throw DcDeterminismError("node is required on PATH for the host smoke test of the wasm builds (this host owns wasm)");
    }

    // The toolchain: recorded by a rebuild, compared first by the check (the platforms of this host only).
    Json toolchain = Json::object();
    if (windows_host) toolchain[kWindowsKey] = windows_toolchain(root, options.worker_only);
    toolchain[target.platform] = docker_toolchain(image, image_id);
    if (wasm.owned) toolchain[kWasmKey] = docker_toolchain(options.emsdk_image, wasm.emsdk_id);
    if (options.check) {
        std::vector<std::string> problems = toolchain_problems(root, toolchain, true, players, wasm.owned);
        if (!problems.empty()) return toolchain_refusal(problems);
    }

    // The entries this host writes (and, in the check, compares).
    std::vector<std::string> binary_prefixes;
    if (windows_host) binary_prefixes.push_back(kWindowsPlatform);
    binary_prefixes.push_back(target.platform);
    std::vector<std::string> player_prefixes = binary_prefixes;
    if (wasm.owned) player_prefixes.push_back("wasm");
    ManifestUpdate update;
    update.source = worker_source_json(source_sha);
    update.toolchain = toolchain;
    if (stale.worker) update.removed_binaries.push_back(kArm64Key);
    if (stale.any()) update.removed_toolchain.push_back(kArm64Key);

    // The builds, from the copy of the sources at the fixed path (every build starts from nothing).
    BuildScratch scratch(root);
    const fs::path& build_root = scratch.root();
    const fs::path out = scratch.path();
    std::optional<BuiltBinary> windows_worker;
    if (windows_host) windows_worker = build_windows_worker(build_root, out, source_sha);
    const BuiltBinary* windows_worker_ptr = windows_worker ? &*windows_worker : nullptr;
    BuiltBinary linux_worker = build_linux_worker(build_root, out, image, source_sha, target);
    Json worker_binaries = describe_workers(windows_worker_ptr,
                                            windows_host ? recorded_flags(windows_worker_flags(), "gcc", build_root) : std::vector<std::string>(),
                                            linux_worker, target, image, image_id);
    update.binaries = worker_binaries;

    if (options.worker_only) {
        if (options.check) {
            CheckReport report;
            check_workers(report, root, windows_worker_ptr, linux_worker, target);
            check_main_manifest(report, existing, worker_source_json(source_sha), worker_binaries, Json(nullptr), binary_prefixes, player_prefixes);
            report.print();
            return report.exit_code();
        }
        // (c) the merged manifest in memory and its check
        Json merged = merge_manifest(existing, update);
        check_merged(existing, merged, update);
        // (d) installation, (e) the manifest and its re-read check, (f) the removed arm64 files
        install_workers(root, windows_worker_ptr, linux_worker, target, worker_binaries);
        write_manifest(root, merged);
        check_written(root, existing, merged, update);
        remove_stale_arm64(root, stale);
        log("worker only: the players section is unchanged");
        return 0;
    }

    // The loaders are built before the WebAssembly dc_worker, whose vendored sources are verified against the identity
    // of these loaders (in memory: nothing is installed before everything was built).
    LoaderSet loaders;
    if (windows_host) {
        loaders.windows_toolchain = toolchain[kWindowsKey];
        build_windows_loaders(build_root, out, loaders);
    }
    build_linux_loaders(build_root, out, image, image_id, target, loaders);
    WasmWorkerBuild wasm_worker;
    if (wasm.owned) {
        WasmSources wasm_src = wasm_sources(root, false, loader_ids);
        wasm_worker = build_wasm_worker(build_root, out, options.emsdk_image, wasm_src);
    }

    PlayerSet set;
    if (windows_host) build_windows_players(build_root, out, set);
    build_linux_players(build_root, out, image, true, target, set);
    if (wasm.owned) build_wasm_players(build_root, out, options.emsdk_image, set);
    verify_smoke(set);

    Json players_json = Json::object();
    players_json["source"] = players_source_json(identity);
    players_json["binaries"] = players_binaries(set);

    if (options.check) {
        CheckReport report;
        check_workers(report, root, windows_worker_ptr, linux_worker, target);
        check_loaders(report, root, loaders, loader_ids);
        if (wasm.owned) check_wasm_bundle(report, root, wasm_worker);
        check_players(report, root, set);
        check_main_manifest(report, existing, worker_source_json(source_sha), worker_binaries, players_json, binary_prefixes, player_prefixes);
        report.print();
        return report.exit_code();
    }

    update.players_source = players_source_json(identity);
    update.player_entries = players_binaries(set);
    for (const std::string& prefix : binary_prefixes) update.replaced_player_prefixes.push_back(prefix + "-");
    if (wasm.owned) update.replaced_player_prefixes.push_back("wasm/");
    if (stale.players) update.replaced_player_prefixes.push_back(kArm64PlayerPrefix);
    Json merged = merge_manifest(existing, update);
    check_merged(existing, merged, update);
    install_loaders(root, loaders, loader_ids);
    install_workers(root, windows_worker_ptr, linux_worker, target, worker_binaries);
    if (wasm.owned) install_wasm_worker(root, wasm_worker);
    // The Windows host replaces every player directory except the carried ones (linux-arm64, and wasm when it is not
    // rebuilt); the other hosts replace only the directories of their own platforms.
    bool wasm_owned = wasm.owned;
    std::vector<std::string> own_dirs = binary_prefixes;
    if (wasm.owned) own_dirs.push_back("wasm");
    install_players(root, set, [&](const std::string& name) {
        if (!windows_host) return has_prefix(name, own_dirs);
        if (starts_with(name, kArm64PlayerPrefix)) return false;  // carried (a stale one is removed below)
        if (starts_with(name, "wasm") && !wasm_owned) return false;
        return true;
    });
    write_manifest(root, merged);
    check_written(root, existing, merged, update);
    remove_stale_arm64(root, stale);
    return 0;
}

}  // namespace

int run_windows_prebuild(const PrebuildOptions& options) { return run_host_prebuild(options, true, linux_target_of_arch("amd64")); }

int run_linux_prebuild(const PrebuildOptions& options) {
    check_prebuild_options(options);
    return run_host_prebuild(options, false, linux_target_of_machine(host_machine()));
}

// --- the tool section: `prebuild --tool-fragment` ---------------------------------------------------------------

namespace {

const char* const kToolSectionKey = "dc_determinism";
const char* const kToolSourceRel = "src/dc_determinism";
const char* const kToolPlatforms[] = {"windows-amd64", "linux-amd64", "linux-arm64"};
// {directory below src/vendor, member of src/vendor/manifest.json}: the dependencies of the tool.
const std::pair<const char*, const char*> kToolDependencies[] = {{"nlohmann-json", "dependencies"}, {"miniz", "dc_determinism_dependencies"}};

std::string tool_binary_rel(const std::string& platform) {
    return "prebuilt/dc_determinism/" + platform + "/dc_determinism" + (platform == kWindowsKey ? ".exe" : "");
}

struct ToolFragment {
    std::string platform;
    Json entry;       // the `binaries` entry: path, sha256, compiler, flags, link_flags, [image, image_id]
    Json toolchain;   // the toolchain member of the platform
    fs::path built;   // the freshly built binary (absolute); empty: the fragment names none
};

ToolFragment read_tool_fragment(const fs::path& root, const std::string& file) {
    auto fail = [&](const std::string& what) { throw DcDeterminismError("tool fragment " + file + ": " + what); };
    Json value = read_json(resolve(path_from_utf8(file)));
    if (!value.is_object()) fail("not a JSON object");
    ToolFragment fragment;
    fragment.platform = member_text(value, {"platform"});
    bool known = false;
    for (const char* platform : kToolPlatforms) known = known || fragment.platform == platform;
    if (!known) fail("unknown platform '" + fragment.platform + "'");
    const bool is_linux = fragment.platform != kWindowsKey;
    fragment.entry = Json::object();
    for (const char* key : {"path", "sha256", "compiler"}) {
        if (!json_get(value, key).is_string() || json_get(value, key).get<std::string>().empty()) fail(std::string("no ") + key);
        fragment.entry[key] = value[key];
    }
    if (member_text(value, {"path"}) != tool_binary_rel(fragment.platform)) {
        fail("path is " + member_text(value, {"path"}) + ", expected " + tool_binary_rel(fragment.platform));
    }
    for (const char* key : {"flags", "link_flags"}) {
        if (!json_get(value, key).is_array()) fail(std::string("no ") + key + " list");
        fragment.entry[key] = value[key];
    }
    if (is_linux) {
        for (const char* key : {"image", "image_id"}) {
            if (!json_get(value, key).is_string() || json_get(value, key).get<std::string>().empty()) fail(std::string("no ") + key);
            fragment.entry[key] = value[key];
        }
    }
    fragment.toolchain = json_get(value, "toolchain");
    if (!fragment.toolchain.is_object() || fragment.toolchain.empty()) fail("no toolchain facts");
    std::string built = member_text(value, {"built"});
    if (!built.empty()) {
        fs::path relative = path_from_utf8(built);
        if (relative.is_absolute() || to_posix(built).find("..") != std::string::npos) fail("built must be relative to the repository root");
        fragment.built = root / relative;
        std::error_code ec;
        if (!fs::is_regular_file(fragment.built, ec)) fail("the built binary " + native(fragment.built) + " does not exist");
        if (sha256_file(fragment.built) != member_text(value, {"sha256"})) {
            fail("the built binary " + native(fragment.built) + " does not have the recorded sha256");
        }
    }
    return fragment;
}

// `source` of the section: the tree hashes of src/dc_determinism and of the two dependencies it is built from, the
// latter verified against src/vendor/manifest.json.
Json tool_source_json(const fs::path& root) {
    Json manifest = read_json(root / path_from_utf8(kVendorManifestRel));
    Json dependencies = Json::object();
    for (const auto& item : kToolDependencies) {
        std::string name = item.first;
        Json entry = json_get(json_get(manifest, item.second), item.first);
        if (!entry.is_object()) throw DcDeterminismError(std::string(kVendorManifestRel) + " has no entry for " + name);
        std::string actual = tree_sha256_lf(root / "src" / "vendor" / path_from_utf8(name));
        std::string expected = member_text(entry, {"tree_sha256_lf"});
        if (actual != expected) {
            throw DcDeterminismError("src/vendor/" + name + " does not match " + kVendorManifestRel + " (tree_sha256_lf " + actual +
                                     ", expected " + expected + ")");
        }
        Json one = Json::object();
        one["path"] = "src/vendor/" + name;
        one["pin"] = json_get(entry, "pin");
        one["tree_sha256_lf"] = actual;
        dependencies[name] = one;
    }
    Json source = Json::object();
    source["path"] = kToolSourceRel;
    source["tree_sha256_lf"] = tree_sha256_lf(root / path_from_utf8(kToolSourceRel));
    source["dependencies"] = dependencies;
    return source;
}

int check_tool_fragments(const fs::path& root, const Json& recorded, const Json& source, const std::vector<ToolFragment>& fragments) {
    if (!recorded.is_object()) {
        report_line(std::string("the manifest has no ") + kToolSectionKey + " section");
        return 2;
    }
    // The toolchain first: another one ends the run with exit code 2 before any binary is compared.
    std::vector<std::string> problems;
    for (const ToolFragment& fragment : fragments) {
        Json have = json_get(json_get(recorded, "toolchain"), fragment.platform.c_str());
        if (have.is_null()) {
            problems.push_back("toolchain not recorded: the dc_determinism section of prebuilt/manifest.json has no toolchain." + fragment.platform);
            continue;
        }
        std::vector<std::string> differences = json_differences(have, fragment.toolchain, "toolchain." + fragment.platform);
        if (!differences.empty()) problems.push_back("toolchain differs: " + join_text(differences, "; "));
    }
    if (!problems.empty()) {
        for (const std::string& problem : problems) report_line(problem);
        report_line("dc_determinism check: the toolchain is not the recorded one, so the binaries were not rebuilt (exit code 2)");
        return 2;
    }

    CheckReport report;
    for (const ToolFragment& fragment : fragments) {
        std::string label = "binary " + fragment.platform;
        fs::path installed = root / path_from_utf8(member_text(fragment.entry, {"path"}));
        std::error_code ec;
        if (!fs::is_regular_file(installed, ec)) {
            report.differs = true;
            report.add(label + ": differs (no installed binary at prebuilt/dc_determinism/" + fragment.platform + ")");
            continue;
        }
        std::string installed_bytes = read_file(installed);
        std::string installed_sha = sha256_hex(installed_bytes);
        std::string rebuilt_sha = member_text(fragment.entry, {"sha256"});
        if (installed_sha == rebuilt_sha) {
            report.add(label + ": identical");
            continue;
        }
        report.differs = true;
        report.add(label + ": differs (installed " + installed_sha.substr(0, 12) + ", rebuilt " + rebuilt_sha.substr(0, 12) + ")");
        if (!fragment.built.empty()) {
            report.add(label + ": " + code_comparison_text(compare_code_sections(installed_bytes, read_file(fragment.built))));
        }
    }
    // The manifest, member by member (not byte by byte).
    std::vector<std::pair<std::string, std::pair<Json, Json>>> members;
    members.push_back({std::string(kToolSectionKey) + ".source", {json_get(recorded, "source"), source}});
    for (const ToolFragment& fragment : fragments) {
        members.push_back({std::string(kToolSectionKey) + ".binaries." + fragment.platform,
                           {json_get(json_get(recorded, "binaries"), fragment.platform.c_str()), fragment.entry}});
    }
    for (const auto& member : members) {
        std::vector<std::string> differences = json_differences(member.second.first, member.second.second, member.first);
        if (differences.empty()) {
            report.add("manifest " + member.first + ": identical");
        } else {
            report.differs = true;
            report.add("manifest " + member.first + ": differs (" + differing_paths(differences) + ")");
        }
    }
    report.print();
    return report.exit_code();
}

}  // namespace

int run_tool_fragments(const PrebuildOptions& options) {
    check_prebuild_options(options);
    if (options.tool_fragments.empty()) throw DcDeterminismError("run_tool_fragments needs at least one --tool-fragment");
    fs::path root = repo_root_of(options);
    const bool windows = windows_host_now();
    std::vector<ToolFragment> fragments;
    for (const std::string& file : options.tool_fragments) {
        ToolFragment fragment = read_tool_fragment(root, file);
        for (const ToolFragment& other : fragments) {
            if (other.platform == fragment.platform) throw DcDeterminismError("two tool fragments for " + fragment.platform);
        }
        fragments.push_back(std::move(fragment));
    }
    Json existing = read_manifest(root);
    check_schema(existing);
    Json recorded = json_get(existing, kToolSectionKey);
    Json source = tool_source_json(root);
    log(std::string(kToolSourceRel) + " tree sha256: " + member_text(source, {"tree_sha256_lf"}));
    if (options.check) return check_tool_fragments(root, recorded, source, fragments);

    // The entries of the platforms without a fragment stay while the source is the recorded one. Another source: the
    // Windows host drops them (with their files); every other host stops (it cannot rebuild the other platforms).
    bool same_source = recorded.is_object() && canonical_json(json_get(recorded, "source")) == canonical_json(source);
    if (!windows && !same_source) {
        throw DcDeterminismError(std::string("the ") + kToolSectionKey + " source (" + kToolSourceRel +
                                 " and its dependencies) differs from the section of prebuilt/manifest.json" +
                                 (recorded.is_object() ? "" : " (it has none)") +
                                 ": only the Windows host rebuilds every platform; run scripts/prebuild.ps1 there");
    }
    // A recorded key that is no platform of this tool is not silently dropped (nor its directory removed): refuse.
    for (const char* member : {"binaries", "toolchain"}) {
        Json recorded_member = json_get(recorded, member);
        if (!recorded_member.is_object()) continue;
        for (auto it = recorded_member.begin(); it != recorded_member.end(); ++it) {
            bool known = false;
            for (const char* platform : kToolPlatforms) known = known || it.key() == platform;
            if (!known) {
                throw DcDeterminismError(std::string("the ") + kToolSectionKey + " section of prebuilt/manifest.json has the unknown " +
                                         member + " key '" + it.key() + "' (known platforms: windows-amd64, linux-amd64, linux-arm64); "
                                         "nothing was changed");
            }
        }
    }
    Json binaries = Json::object();
    Json toolchain = Json::object();
    if (same_source) {
        if (json_get(recorded, "binaries").is_object()) binaries = json_get(recorded, "binaries");
        if (json_get(recorded, "toolchain").is_object()) toolchain = json_get(recorded, "toolchain");
    }
    for (const ToolFragment& fragment : fragments) {
        binaries[fragment.platform] = fragment.entry;
        toolchain[fragment.platform] = fragment.toolchain;
    }
    auto in_platform_order = [](const Json& object) {
        Json out = Json::object();
        for (const char* platform : kToolPlatforms) {
            if (object.contains(platform)) out[platform] = object[platform];
        }
        return out;
    };
    std::vector<std::string> dropped_platforms;
    if (recorded.is_object() && !same_source && json_get(recorded, "binaries").is_object()) {
        for (auto it = recorded["binaries"].begin(); it != recorded["binaries"].end(); ++it) {
            if (!binaries.contains(it.key())) dropped_platforms.push_back(it.key());
        }
    }
    Json section = Json::object();
    section["source"] = source;
    section["toolchain"] = in_platform_order(toolchain);
    section["binaries"] = in_platform_order(binaries);

    // The manifest: the section replaces the section of the manifest (in its place), else it is appended.
    Json merged = Json::object();
    bool placed = false;
    for (auto it = existing.begin(); it != existing.end(); ++it) {
        if (it.key() == kToolSectionKey) {
            merged[kToolSectionKey] = section;
            placed = true;
        } else {
            merged[it.key()] = it.value();
        }
    }
    if (!placed) merged[kToolSectionKey] = section;
    auto others_equal = [&](const Json& value, const std::string& when) {
        for (auto it = existing.begin(); it != existing.end(); ++it) {
            if (it.key() == kToolSectionKey) continue;
            if (!value.contains(it.key()) || value[it.key()] != it.value()) {
                throw DcDeterminismError("internal check " + when + " failed: the " + it.key() + " member changed");
            }
        }
    };
    others_equal(merged, "before writing the manifest");

    // Installation: the freshly built binaries (a fragment without one must find its binary installed), then the manifest.
    for (const ToolFragment& fragment : fragments) {
        fs::path target = root / path_from_utf8(member_text(fragment.entry, {"path"}));
        if (!fragment.built.empty()) {
            copy_replacing(fragment.built, target);
            if (fragment.platform != kWindowsKey) make_executable(target);
        }
        std::error_code ec;
        if (!fs::is_regular_file(target, ec) || sha256_file(target) != member_text(fragment.entry, {"sha256"})) {
            throw DcDeterminismError("the installed " + member_text(fragment.entry, {"path"}) + " is not the built binary");
        }
    }
    write_manifest(root, merged);
    Json reread = read_manifest(root);
    if (reread != merged) throw DcDeterminismError("internal check after writing the manifest failed: it does not read back as written");
    others_equal(reread, "after writing the manifest");
    // The platforms dropped with the changed source: their binaries go last, when the manifest no longer names them.
    for (const std::string& platform : dropped_platforms) {
        std::error_code ec;
        fs::remove_all(root / "prebuilt" / "dc_determinism" / platform, ec);
        if (ec) throw DcDeterminismError("cannot remove prebuilt/dc_determinism/" + platform + ": " + ec.message());
        log("the " + platform + " dc_determinism binary was removed: it was built from another source; rebuild it on a " + platform + " host");
    }
    log(std::string("updated the ") + kToolSectionKey + " section of the manifest (platforms: " +
        join_text([&] { std::vector<std::string> names; for (auto it = section["binaries"].begin(); it != section["binaries"].end(); ++it) names.push_back(it.key()); return names; }(), ", ") + ")");
    return 0;
}

int run_prebuild(const PrebuildOptions& options) {
    check_prebuild_options(options);
    if (!options.tool_fragments.empty()) return run_tool_fragments(options);
    if (options.wasm_worker_only) return run_wasm_worker_prebuild(options);
    return windows_host_now() ? run_windows_prebuild(options) : run_linux_prebuild(options);
}

}  // namespace dc_determinism
