// `--compiler wasm`: the verification of the prebuilt WebAssembly bundle against its manifest (a synthetic
// repository root; tool_fake_worker answers `--version` in place of node), the option rules, the
// command and loader blocks, and one integration test of the committed bundle that is skipped without
// node or the bundle (on linux-arm64 also without that platform's loader manifest). The wasm worker is not
// available through the pybind worker.
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/platform.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"
#include "sim/wasm.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const std::exception& error) {
        return error.what();
    }
    return std::string();
}

void write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The stand-in for node: it answers `--version` with `dc_worker <64 x 'a'>` whatever the script.
std::string fake_node() {
    return path_to_utf8(executable_path().parent_path() / (is_windows() ? "tool_fake_worker.exe" : "tool_fake_worker"));
}

const std::string kSource(64, 'a');

// A synthetic repository root: the wasm bundle, the native worker manifest and the loader manifest of the
// host platform, consistent with each other.
struct Root {
    testing::TempDir dir;
    Json manifest;
    Json native;
    Json loader;

    fs::path bundle() const { return dir.path() / "prebuilt" / "worker" / "wasm"; }
    fs::path loader_manifest() const {
        return dir.path() / "prebuilt" / "loaders" / platform_key() / "manifest.json";
    }

    Root() {
        write_file(bundle() / "dc_worker.js", "js-code");
        write_file(bundle() / "dc_worker.wasm", "wasm-code");
        Json simulator = Json::object({{"source", Json::object({{"revision", "r1"}, {"tree_sha256_lf", std::string(64, '1')}})},
                                       {"dependencies", Json::object({{"box2d", std::string(64, '2')}})}});
        manifest = Json::object();
        manifest["schema"] = kWasmSchema;
        manifest["worker"] = Json::object({{"path", "prebuilt/worker/wasm/dc_worker.js"},
                                           {"sha256", sha256_hex("js-code")},
                                           {"sidecars", Json::object({{"dc_worker.wasm", sha256_hex("wasm-code")}})}});
        manifest["source"] = Json::object({{"path", "src/worker/worker.cpp"}, {"sha256", kSource}});
        manifest["simulator"] = simulator;
        manifest["toolchain"] = Json::object({{"emxx", "em++ 6.0.9"}, {"image", "emsdk"}, {"image_id", "sha256:1"}});
        manifest["flags"] = Json::object({{"link", Json::array({"-O3"})}});
        manifest["build"] = Json::object({{"override_sha256", std::string(64, '3')},
                                          {"shim_sha256", Json::object({{"dcw_wasm_shim.cpp", std::string(64, '4')}})}});
        native = Json::object({{"schema", "dc-determinism-prebuilt-v2"},
                               {"source", Json::object({{"path", "src/worker/worker.cpp"}, {"sha256", kSource}})}});
        loader = Json::object({{"source", simulator["source"]}, {"dependencies", simulator["dependencies"]}});
        save();
    }

    void save() {
        write_file(bundle() / "manifest.json", json_dumps(manifest));
        write_file(dir.path() / "prebuilt" / "manifest.json", json_dumps(native));
        write_file(loader_manifest(), json_dumps(loader));
    }

    WasmOptions options() const {
        WasmOptions options;
        options.root = dir.path();
        options.node = fake_node();
        return options;
    }

    std::string failure() const {
        WasmOptions given = options();
        return error_of([&] { verify_wasm_bundle(given); });
    }
};

bool starts_with(const std::string& text, const std::string& prefix) { return text.compare(0, prefix.size(), prefix) == 0; }

// Restores PATH when a test that empties it ends.
struct PathGuard {
    std::string saved = env_get("PATH").value_or("");
    void set(const std::string& value) {
#ifdef _WIN32
        _putenv_s("PATH", value.c_str());
#else
        setenv("PATH", value.c_str(), 1);
#endif
    }
    ~PathGuard() { set(saved); }
};

}  // namespace

TEST(wasm_bundle_refuses_other_schema_names) {
    Root current;
    CHECK_EQ(current.failure(), std::string());
    Root other_wasm;
    other_wasm.manifest["schema"] = "dc-determinism-wasm-worker-v2";
    other_wasm.save();
    CHECK(starts_with(other_wasm.failure(), "invalid wasm worker manifest: "));
    Root other_native;
    other_native.native["schema"] = "dc-determinism-prebuilt-v3";
    other_native.save();
    CHECK(starts_with(other_native.failure(), "invalid worker manifest: "));
}
TEST(wasm_bundle_is_described_for_results) {
    Root root;
    WasmBundle bundle = verify_wasm_bundle(root.options());
    CHECK_EQ(bundle.command.size(), static_cast<std::size_t>(2));
    CHECK_EQ(bundle.command[0], fake_node());
    CHECK(bundle.command[1].find("dc_worker.js") != std::string::npos);
    CHECK_EQ(bundle.worker["sha256"].get<std::string>(), sha256_hex("js-code"));
    CHECK_EQ(bundle.worker["version"].get<std::string>(), "dc_worker " + kSource);
    CHECK_EQ(bundle.worker["manifest_entry"]["compiler"].get<std::string>(), std::string("em++ 6.0.9"));
    CHECK_EQ(bundle.worker["manifest_entry"]["sidecars"]["dc_worker.wasm"].get<std::string>(), sha256_hex("wasm-code"));
    CHECK(bundle.worker["runtime"].contains("node"));

    const Json& info = bundle.loader.info;
    CHECK_EQ(bundle.loader.path.filename().string(), std::string("dc_worker.wasm"));
    CHECK_EQ(info["path"].get<std::string>(), path_text(bundle.loader.path));
    CHECK_EQ(info["sha256"].get<std::string>(), sha256_hex("wasm-code"));
    CHECK_EQ(info["compiler"].get<std::string>(), std::string("wasm"));
    CHECK_EQ(info["platform"].get<std::string>(), std::string("wasm32-emscripten"));
    CHECK_EQ(info["origin"].get<std::string>(), std::string("prebuilt"));
    const Json& prebuilt = info["dc_prebuilt"];
    CHECK(prebuilt["source"] == root.loader["source"] && prebuilt["dependencies"] == root.loader["dependencies"]);
    CHECK_EQ(prebuilt["library"]["filename"].get<std::string>(), std::string("dc_worker.wasm"));
    CHECK_EQ(prebuilt["library"]["target"].get<std::string>(), std::string("wasm32-emscripten"));
    CHECK_EQ(prebuilt["wasm_build"]["override_sha256"].get<std::string>(), std::string(64, '3'));
    CHECK_EQ(prebuilt["wasm_build"]["manifest_sha256"].get<std::string>(),
             sha256_hex(read_file(root.bundle() / "manifest.json")));

    // A native loader built from the same sources has the same source identity as the bundle.
    Json native = Json::object({{"origin", "prebuilt"},
                                {"dc_prebuilt", Json::object({{"source", root.loader["source"]},
                                                              {"dependencies", root.loader["dependencies"]}})}});
    CHECK(check_source_identity(info, native)["verified"].get<bool>());
    native["dc_prebuilt"]["dependencies"]["box2d"] = std::string(64, '9');
    CHECK(starts_with(error_of([&] { check_source_identity(info, native); }), "incompatible results"));
}

TEST(wasm_bundle_tampering_is_named) {
    {
        Root root;
        write_file(root.bundle() / "dc_worker.wasm", "other");
        CHECK(starts_with(root.failure(), "wasm module hash mismatch: "));
    }
    {
        Root root;
        write_file(root.bundle() / "dc_worker.js", "other");
        CHECK(starts_with(root.failure(), "wasm worker hash mismatch: "));
    }
    {
        Root root;
        fs::remove(root.bundle() / "dc_worker.wasm");
        CHECK(starts_with(root.failure(), "wasm module not found: "));
    }
    {
        Root root;
        fs::remove(root.bundle() / "dc_worker.js");
        CHECK(starts_with(root.failure(), "wasm worker not found: "));
    }
    {
        Root root;
        fs::remove(root.bundle() / "manifest.json");
        CHECK(starts_with(root.failure(), "wasm worker manifest not found: "));
    }
    {
        Root root;
        root.manifest["schema"] = "dc-determinism-wasm-worker-v0";
        root.save();
        CHECK(starts_with(root.failure(), "invalid wasm worker manifest: "));
    }
    for (const char* member : {"worker", "source", "simulator", "toolchain", "build"}) {
        Root root;
        root.manifest.erase(member);
        root.save();
        CHECK(starts_with(root.failure(), "invalid wasm worker manifest: "));
    }
    {
        Root root;
        root.manifest["worker"]["sidecars"] = Json::object();
        root.save();
        CHECK(starts_with(root.failure(), "invalid wasm worker manifest: "));
    }
    for (const char* path : {"../dc_worker.js", "prebuilt/worker/wasm/other.js", "/etc/dc_worker.js", "worker/dc_worker.js"}) {
        Root root;
        root.manifest["worker"]["path"] = path;
        root.save();
        CHECK(starts_with(root.failure(), "unsafe wasm worker path in the manifest: "));
    }
}

TEST(wasm_bundle_must_match_the_repository) {
    {
        // The worker source recorded by the bundle differs from prebuilt/manifest.json.
        Root root;
        root.native["source"]["sha256"] = std::string(64, 'b');
        root.save();
        CHECK(starts_with(root.failure(), "stale wasm bundle: built from worker source " + kSource));
    }
    {
        // ... and from src/worker/worker.cpp when that file is present (its LF hash is compared).
        Root root;
        write_file(root.dir.path() / "src" / "worker" / "worker.cpp", "int main() {}\r\n");
        std::string error = root.failure();
        CHECK(starts_with(error, "stale wasm bundle: built from worker source " + kSource));
        CHECK(error.find("src/worker/worker.cpp is " + sha256_hex("int main() {}\n")) != std::string::npos);
    }
    {
        Root root;
        root.loader["source"]["tree_sha256_lf"] = std::string(64, '8');
        root.save();
        CHECK(starts_with(root.failure(), "wasm simulator source mismatch: "));
    }
    {
        Root root;
        root.loader["dependencies"]["box2d"] = std::string(64, '8');
        root.save();
        CHECK(starts_with(root.failure(), "wasm simulator source mismatch: "));
    }
    {
        Root root;
        fs::remove(root.loader_manifest());
        CHECK(starts_with(root.failure(), "loader manifest not found: "));
    }
    {
        // The recorded worker source is not what the worker reports: version mismatch.
        Root root;
        root.manifest["source"]["sha256"] = std::string(64, 'c');
        root.native["source"]["sha256"] = std::string(64, 'c');
        root.save();
        CHECK(starts_with(root.failure(), "wasm worker version mismatch: "));
    }
}

TEST(wasm_worker_requires_node_on_path) {
    Root root;
    testing::TempDir empty;
    PathGuard guard;
    guard.set(path_text(empty.path()));
    WasmOptions options = root.options();
    options.node.clear();
    CHECK_EQ(error_of([&] { verify_wasm_bundle(options); }), std::string("the wasm worker requires node on PATH"));
    // The hook of `--worker-command` starts nothing through node: no node, no version.
    options.check_node = false;
    WasmBundle bundle = verify_wasm_bundle(options);
    CHECK(bundle.command.empty() && bundle.worker["version"].is_null());
    CHECK_EQ(bundle.loader.info["sha256"].get<std::string>(), sha256_hex("wasm-code"));
}

TEST(wasm_has_no_build_or_dc_root_option) {
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    for (const char* option : {"--build", "--dc-root"}) {
        bool rejected = false;
        try {
            parser->parse({"verify", "--compiler", "wasm", option});
        } catch (const cli::ParseExit&) {
            rejected = true;
        }
        CHECK(rejected);
    }
}

TEST(wasm_is_a_compiler_choice_of_the_simulator_commands) {
    auto accepted = [](const std::string& line) {
        std::unique_ptr<cli::Parser> parser = cli::build_parser();
        std::vector<std::string> words;
        std::size_t start = 0;
        while (start < line.size()) {
            std::size_t end = line.find(' ', start);
            if (end == std::string::npos) end = line.size();
            if (end > start) words.push_back(line.substr(start, end - start));
            start = end + 1;
        }
        try {
            parser->parse(words);
            return true;
        } catch (const cli::ParseExit&) {
            return false;
        }
    };
    for (const char* line : {"verify --compiler wasm", "verify --compiler wasm --verbose",
                             "run simulator --prepared p --compiler wasm --engine fcv1 --output r.json",
                             "benchmark --prepared p --compiler wasm --output b.json",
                             "capture simulator --prepared p --engine fcv1 --compiler wasm --all --output c.zip",
                             "compare engines --prepared p --compiler wasm --output e.json",
                             "compare engines --prepared p --compiler wasm --workers 2 --output e.json"}) {
        if (!accepted(line)) ::testing::report_failure(__FILE__, __LINE__, std::string("rejected: ") + line);
    }
    // The pybind comparison is native Linux only.
    CHECK(!accepted("compare pybind --prepared p --logs-dir l --compiler wasm --output e.json"));
}

namespace {

// The three shots of src/worker/wasm/smoke-input.bin (prepared positions 3641, 10076 and 13971 of the
// full corpus), as `<position> <status> <steps> <trajectory digest> <final digest>`. Native x86-64 Windows
// and Linux builds (msvc, gcc and clang) give the same digests for them; the wasm digests are the same on
// every host, the native ones are compared with these constants on x86-64 only.
struct Expected {
    const char* engine;
    const char* lines[3];
};
const Expected kExpected[] = {
    {"fcv1",
     {"0 stopped 5214 e4423cdaf756df4a3887934723c71f12876310921930c1e5c08eba16790a87fe "
      "8e86f6182ac2ba976eef5c9cdb912af0513001a1ee5e66b4caf240feccf64548",
      "1 stopped 8337 2dfbddc139a760649c7d65707af92109c9a9d698568b4a72fc35de3d5d6e30ba "
      "e88c0456c4fd077146f2bfff6778d3531018fad8592ad0e155dec62c9bc13451",
      "2 stopped 5247 140872eb76e36d59f4481e355c5c9284551fd7f7f9d9442280a8113c332ede98 "
      "788c0ee1a754a2745865257971215a376f89881af46113da44adbf61edd5e052"}},
    {"fcv1plus",
     {"0 stopped 5214 28cdfc40353e299aa2cad4af22b5bcf8bb5cd0b59d848cd11642fe3c780f5764 "
      "fb8f50519d643c404f22859b16a073a6b818a9f2ba7c9c7b11982c911157651d",
      "1 stopped 8337 5dde29155d1ed4b10b57fcc4c6fe26d69112b89b3cac9f136e679a990ee1ded3 "
      "ded753f825418ab75688eabbc3a36133aaa7ac9989f8e1cc94cd27395ff434a9",
      "2 stopped 5247 9c99ba1eada0cb04c9155196d60cf8ae3406574cded46015590199ff9bc199f1 "
      "788c0ee1a754a2745865257971215a376f89881af46113da44adbf61edd5e052"}},
};

std::string outcome_line(std::size_t position, const Outcome& outcome) {
    return std::to_string(position) + " " + outcome.status + " " + std::to_string(outcome.steps) + " " +
           outcome.trajectory_digest + " " + outcome.final_digest;
}

std::vector<std::string> shot_command(const std::vector<std::string>& prefix, const fs::path& loader,
                                      const std::string& engine, const fs::path& input) {
    std::vector<std::string> command = prefix;
    for (const std::string& part : {std::string("--loader"), path_text(loader), std::string("--engine"), engine,
                                    std::string("--input"), path_text(input), std::string("--max-steps"),
                                    std::string("60000")}) {
        command.push_back(part);
    }
    return command;
}

}  // namespace

// The committed bundle through the command prefix and loader of `--compiler wasm`: reproducible, the pinned
// digests, and equal to the native worker of this host where one is available.
TEST(wasm_bundle_runs_the_smoke_shots) {
    if (!find_on_path("node").has_value() || !fs::is_regular_file(repo_root() / "prebuilt" / "worker" / "wasm" / "manifest.json")) {
        std::fprintf(stderr, "  skipped: node or the wasm bundle is missing\n");
        return;
    }
    const bool arm64 = std::string(platform_key()) == "linux-arm64";
    if (arm64 && !fs::is_regular_file(repo_root() / "prebuilt" / "loaders" / platform_key() / "manifest.json")) {
        std::fprintf(stderr, "  skipped: prebuilt/loaders/%s/manifest.json is missing\n", platform_key());
        return;
    }
    fs::path input = repo_root() / "src" / "worker" / "wasm" / "smoke-input.bin";
    CHECK(fs::is_regular_file(input));
    SimulatorTarget target = resolve_simulator("wasm", std::string());
    CHECK_EQ(target.loader.info["compiler"].get<std::string>(), std::string("wasm"));
    CHECK_EQ(target.prefix.size(), static_cast<std::size_t>(2));
    CHECK_EQ(target.worker["sha256"].get<std::string>(), sha256_file(path_from_utf8(target.prefix[1])));

    // The native worker and loader of this host, when they can be used.
    std::vector<std::string> native_prefix;
    ResolvedLoader native_loader;
    bool have_native = true;
    try {
        Json worker = verify_default_worker();
        native_prefix = {worker["path"].get<std::string>()};
        native_loader = loader_provenance(is_windows() ? "msvc" : "gcc");
        CHECK(check_source_identity(target.loader.info, native_loader.info)["verified"].get<bool>());
    } catch (const DcDeterminismError& error) {
        // Not silent: the arm64 CTest gate fails on "skipped:" in the output.
        std::fprintf(stderr, "  skipped: native comparison unavailable: %s\n", error.what());
        have_native = false;
    }

    for (const Expected& expected : kExpected) {
        std::vector<std::string> command = shot_command(target.prefix, target.loader.path, expected.engine, input);
        auto first = run_repetition(command, 3, 2, 120.0, nullptr);
        auto second = run_repetition(command, 3, 1, 120.0, nullptr);
        for (std::size_t i = 0; i < 3; ++i) {
            CHECK_EQ(outcome_line(i, first.second[i]), std::string(expected.lines[i]));
            CHECK_EQ(outcome_line(i, second.second[i]), std::string(expected.lines[i]));
        }
        if (!have_native) continue;
        std::vector<std::string> native = shot_command(native_prefix, native_loader.path, expected.engine, input);
        auto reference = run_repetition(native, 3, 1, 120.0, nullptr);
        if (arm64) {
            // Native arm64 digests are not pinned here (they are a result of the experiment): the runs
            // must be reproducible.
            auto again = run_repetition(native, 3, 2, 120.0, nullptr);
            for (std::size_t i = 0; i < 3; ++i) {
                CHECK_EQ(outcome_line(i, again.second[i]), outcome_line(i, reference.second[i]));
            }
        } else {
            for (std::size_t i = 0; i < 3; ++i) CHECK_EQ(outcome_line(i, reference.second[i]), std::string(expected.lines[i]));
        }
    }
}

int main() { return testing::run_all_tests(); }
