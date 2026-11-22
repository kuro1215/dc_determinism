// The local build recipe library (used by `prebuild`): vendored-source identity, receipts and cache decisions, with the
// toolchain replaced by fakes (no compiler is run).
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "localbuild/local_build.hpp"
#include "player/binaries.hpp"
#include "sim/compilers.hpp"
#include "sim/provenance.hpp"
#include "testing.hpp"
#include "tempdir.hpp"

using namespace dc_determinism;

namespace {

fs::path repo() { return fs::path(DC_DETERMINISM_REPO_ROOT_DIR); }

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const DcDeterminismError& error) {
        return error.what();
    }
    return std::string();
}

void write_bytes(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string library_name() {
    return is_windows() ? "libdigitalcurling_plugin_loader.dll" : "libdigitalcurling_plugin_loader.so";
}

// A toolchain that creates the files a real build would create.
struct FakeToolchain {
    explicit FakeToolchain(const fs::path& cache) {
        build_hooks().cache = cache;
        build_hooks().probe = [this](const std::string& kind, const std::string& compiler) {
            probed.push_back(kind + "/" + compiler);
            if (!probe_error.empty()) {
                Json failure = Json::object();
                failure["ok"] = false;
                failure["error"] = probe_error;
                return failure;
            }
            Json answer = Json::object();
            answer["ok"] = true;
            answer["id"] = compiler;
            answer["cxx"] = "/tool/bin/g++";
            answer["cc"] = "/tool/bin/gcc";
            answer["version"] = version;
#if defined(__linux__) && defined(__aarch64__)
            answer["target"] = "aarch64-linux-gnu";  // the fake toolchain follows the host
#else
            answer["target"] = "x86_64-linux-gnu";
#endif
            answer["bin_dir"] = path_to_utf8(bin_dir);
            answer["generator"] = kind == "loader" ? Json("Ninja") : Json(nullptr);
            answer["generator_args"] = Json::array();
            if (kind == "loader") {
                answer["cmake"] = "/x/cmake";
                answer["cmake_version"] = "cmake version 3.28.3";
            }
            if (compiler == "wasm") {
                answer["node"] = "/usr/bin/node";
                answer["node_version"] = "v20.0.0";
            }
            return answer;
        };
        build_hooks().run = [this](const std::string& what, const std::string&, const std::string& compiler,
                                   const fs::path&, const std::vector<std::string>& argv) {
            commands.push_back(argv);
            run_compilers.push_back(compiler);
            ++calls;
            auto after = [&](const char* flag) -> std::string {
                for (std::size_t i = 0; i + 1 < argv.size(); ++i) {
                    if (argv[i] == flag) return argv[i + 1];
                }
                return std::string();
            };
            std::string build_dir = after("--build");
            if (!build_dir.empty()) {
                if (fail_build) throw DcDeterminismError(what + " failed (exit code 1):\ncmake build failed");
                if (create_output) {
                    write_bytes(path_from_utf8(build_dir) / "src" / "plugin-loader" / library_name(),
                                "loader-" + std::to_string(calls));
                }
                return;
            }
            std::string output = after("-o");
            if (!output.empty() && create_output) {
                fs::path path = path_from_utf8(output);
                write_bytes(path, "player-" + std::to_string(calls));
                fs::permissions(path, fs::perms::owner_all, fs::perm_options::add);
                if (path.extension() == ".js") {
                    fs::path wasm = path;
                    wasm.replace_extension(".wasm");
                    write_bytes(wasm, "wasm");
                }
            }
        };
    }
    ~FakeToolchain() {
        build_hooks().probe = nullptr;
        build_hooks().run = nullptr;
        build_hooks().cache = fs::path();
    }

    std::string version = "g++ 1.0";
    std::string probe_error;
    fs::path bin_dir = "/tool/bin";
    bool fail_build = false;
    bool create_output = true;
    int calls = 0;
    std::vector<std::string> probed;
    std::vector<std::string> run_compilers;  // the compiler id each command was run for
    std::vector<std::vector<std::string>> commands;
};

}  // namespace

TEST(vendored_trees_match_the_recorded_hashes) {
    Json sources = loader_sources(repo());
    Json revisions = read_json(repo() / "src" / "vendor" / "manifest.json");
    CHECK_EQ(sources["simulator"]["tree_sha256_lf"], revisions["digitalcurling"]["tree_sha256_lf"]);
    CHECK_EQ(sources["simulator"]["revision"], revisions["digitalcurling"]["revision"]);
    const char* const names[] = {"fpm", "nlohmann-json", "box2d", "xxhash"};
    CHECK_EQ(sources["dependencies"].size(), static_cast<std::size_t>(4));
    std::size_t index = 0;
    for (auto it = sources["dependencies"].begin(); it != sources["dependencies"].end(); ++it, ++index) {
        CHECK_EQ(it.key(), std::string(names[index]));
        CHECK_EQ(it.value()["git_tree"].get<std::string>().size(), static_cast<std::size_t>(40));
    }
    Json player = player_sources(repo());
    CHECK_EQ(player["player_library"]["tree_sha256_lf"], revisions["digitalcurling"]["tree_sha256_lf"]);
    CHECK_EQ(player["worker_cpp_sha256_lf"].get<std::string>().size(), static_cast<std::size_t>(64));
    CHECK_EQ(player["dependencies"].size(), static_cast<std::size_t>(2));
}

TEST(a_tampered_tree_is_rejected) {
    testing::TempDir dir;
    fs::copy(repo() / "src" / "vendor" / "xxhash", dir.path() / "xxhash", fs::copy_options::recursive);
    std::string message = error_of([&] { verified_tree(dir.path() / "xxhash", std::string(64, '0'), "dependency xxhash"); });
    CHECK(message.find("vendored dependency xxhash does not match its recorded tree hash (expected " +
                       std::string(64, '0') + ", found ") == 0);
    CHECK(error_of([&] { verified_tree(dir.path() / "absent", std::string(64, '0'), "dependency absent"); })
              .find("vendored dependency absent is missing: ") == 0);
    std::string actual = verified_tree(repo() / "src" / "vendor" / "xxhash", read_json(repo() / "src" / "vendor" / "manifest.json")
                                                                                  ["dependencies"]["xxhash"]["tree_sha256_lf"],
                                       "dependency xxhash");
    CHECK_EQ(actual.size(), static_cast<std::size_t>(64));
}

TEST(box2d_gets_the_cmake_patch_in_a_copy) {
    testing::TempDir dir;
    fs::path patched = prepare_box2d(repo(), dir.path());
    CHECK_EQ(patched.filename().string(), std::string("box2d-patched"));
    std::string text = read_file(patched / "CMakeLists.txt");
    CHECK(text.find("cmake_minimum_required(VERSION 3.15)") != std::string::npos);
    CHECK(text != read_file(repo() / "src" / "vendor" / "box2d" / "CMakeLists.txt"));
    std::size_t original_files = 0, patched_files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(repo() / "src" / "vendor" / "box2d")) {
        original_files += entry.is_regular_file() ? 1 : 0;
    }
    for (const auto& entry : fs::recursive_directory_iterator(patched)) patched_files += entry.is_regular_file() ? 1 : 0;
    CHECK_EQ(original_files, patched_files);
}

TEST(player_flags_follow_the_prebuild_recipe) {
    std::vector<std::string> msvc = player_flags("msvc", "nd2");
    CHECK_EQ(json_dumps(Json(msvc), DumpOptions::compact()),
             std::string("[\"/nologo\",\"/std:c++17\",\"/EHsc\",\"/O2\",\"/DNDEBUG\",\"/wd4996\",\"/utf-8\",\"/DPLAYER_ND2\"]"));
    CHECK_EQ(player_flags("msvc", "normal_dist").size(), static_cast<std::size_t>(7));
    std::vector<std::string> gcc = player_flags("gcc", "nd2");
    CHECK_EQ(gcc[0], std::string("-std=c++17"));
    CHECK_EQ(gcc[4], std::string("-DPLAYER_ND2"));
    CHECK_EQ(gcc.back(), is_windows() ? std::string("-static") : std::string("-static-libgcc"));
    std::vector<std::string> wasm = player_flags("wasm", "normal_dist");
    CHECK_EQ(wasm.size(), static_cast<std::size_t>(8));
    CHECK_EQ(wasm[4], std::string("-fexceptions"));
    CHECK_EQ(wasm.back(), std::string("-sENVIRONMENT=node"));
}

TEST(loader_build_writes_a_receipt_and_reuses_it) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltLoader first = build_loader("gcc", repo());
    CHECK_EQ(first.path, cache.path() / "gcc" / "loader" / "bin" / library_name());
    CHECK_EQ(fake.calls, 2);  // configure + build
    CHECK(path_text(first.path).find(is_windows() ? '/' : '\\') == std::string::npos);
    CHECK_EQ(first.receipt["schema"], Json("dc-determinism-build-receipt-v1"));
    CHECK_EQ(first.receipt["kind"], Json("loader"));
    CHECK_EQ(first.receipt["compiler"]["version"], Json("g++ 1.0"));
    CHECK_EQ(first.receipt["compiler"]["executable"], Json("/tool/bin/g++"));
    CHECK_EQ(first.receipt["output"]["sha256"].get<std::string>(), sha256_file(first.path));
    CHECK_EQ(first.receipt["output"]["file"], Json("bin/" + library_name()));
    Json recorded = loader_sources(repo());
    CHECK_EQ(first.receipt["sources"]["simulator"]["tree_sha256_lf"], recorded["simulator"]["tree_sha256_lf"]);
    CHECK_EQ(first.receipt["flags"]["generator"], Json("Ninja"));
    CHECK_EQ(first.receipt["flags"]["cmake"], Json("cmake version 3.28.3"));
    CHECK(first.receipt["host"]["python"].is_null());
    CHECK_EQ(first.receipt["built_at"].get<std::string>().size(), static_cast<std::size_t>(25));
    bool bundles = false;
    for (const Json& flag : first.receipt["flags"]["configure"]) bundles = bundles || flag == Json("-DDIGITALCURLING_BUNDLE_PLUGINS=ON");
    CHECK(bundles);
    CHECK_EQ(first.receipt["flags"]["build"].back(), Json("--parallel"));
    CHECK_EQ(read_json(cache.path() / "gcc" / "loader" / "build-receipt.json"), first.receipt);
    // The commands: cmake configure, then the build with the job count.
    CHECK_EQ(fake.commands[0][0], std::string("/x/cmake"));
    CHECK_EQ(fake.commands[1][1], std::string("--build"));
    CHECK(!fake.commands[1].back().empty() && std::stoi(fake.commands[1].back()) >= 1);

    BuiltLoader again = build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 2);  // nothing rebuilt
    CHECK_EQ(again.path, first.path);
    CHECK_EQ(again.receipt, first.receipt);
}

TEST(a_present_cache_is_ignored_when_the_build_is_told_not_to_reuse_it) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 2);
    build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 2);  // the default reuses the valid cache
    BuiltLoader fresh = build_loader("gcc", repo(), false);  // what `prebuild` passes
    CHECK_EQ(fake.calls, 4);  // configure and build again, from nothing
    CHECK(fs::is_regular_file(fresh.path));
    CHECK_EQ(fresh.receipt["output"]["sha256"].get<std::string>(), sha256_file(fresh.path));
    build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 4);  // the rebuilt result is a valid cache again
}

TEST(receipt_identity_does_not_depend_on_the_checkout_location) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltLoader built = build_loader("gcc", repo());
    std::string text = json_dumps(built.receipt["identity"]);
    std::string repo_posix = path_to_utf8(resolve(repo()));
    std::replace(repo_posix.begin(), repo_posix.end(), '\\', '/');
    std::string cache_posix = path_to_utf8(cache.path());
    std::replace(cache_posix.begin(), cache_posix.end(), '\\', '/');
    CHECK(text.find(repo_posix) == std::string::npos);
    CHECK(text.find(cache_posix) == std::string::npos);
    CHECK(text.find("<repo>/src/vendor/digitalcurling") != std::string::npos);
    CHECK(text.find("<build>/cmake") != std::string::npos);
}

TEST(a_changed_input_triggers_a_rebuild) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltLoader first = build_loader("gcc", repo());
    fake.version = "g++ 2.0";  // compiler change
    BuiltLoader second = build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 4);
    CHECK_EQ(second.receipt["compiler"]["version"], Json("g++ 2.0"));
    CHECK(first.receipt["output"]["sha256"] != second.receipt["output"]["sha256"]);

    write_bytes(second.path, "tampered");  // modified output
    build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 6);

    write_bytes(cache.path() / "gcc" / "loader" / "build-receipt.json", "{not json");  // corrupt receipt
    build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 8);
}

TEST(a_failed_or_incomplete_build_leaves_no_receipt) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    fake.fail_build = true;
    CHECK(error_of([] { build_loader("gcc", repo()); }).find("cmake build failed") != std::string::npos);
    CHECK(!fs::exists(cache.path() / "gcc" / "loader" / "build-receipt.json"));
    fake.fail_build = false;
    fake.create_output = false;
    CHECK(error_of([] { build_loader("gcc", repo()); }).find("the loader library was not created below ") == 0);
    CHECK(!fs::exists(cache.path() / "gcc" / "loader" / "build-receipt.json"));
}

TEST(probe_errors_are_reported_as_they_are) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    fake.probe_error = "required tool not found: cmake. Install CMake 3.26 or newer and put it on PATH (Windows: winget install Kitware.CMake; Linux: apt install cmake).";
    CHECK_EQ(error_of([] { build_loader("gcc", repo()); }), fake.probe_error);
    CHECK_EQ(fake.calls, 0);
    CHECK_EQ(error_of([] { build_loader("tcc", repo()); }),
             "unknown compiler for a local build: tcc (choose from " + local_compiler_text(false) + ")");
    CHECK_EQ(error_of([] { build_player("nd3", "gcc", repo()); }), std::string("unknown player: nd3 (choose from normal_dist, nd2)"));
    CHECK_EQ(error_of([] { build_player("nd2", "tcc", repo()); }),
             "unknown compiler for a local player build: tcc (choose from " + local_compiler_text(true) + ")");
    CHECK(fake.probed.size() == 1);  // only the first call reached the probe
}

#ifdef _WIN32
TEST(mingw_runtime_dlls_travel_with_the_loader) {
    testing::TempDir cache;
    testing::TempDir compiler_dir;
    for (const char* name : {"libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll"}) {
        write_bytes(compiler_dir.path() / name, name);
    }
    FakeToolchain fake(cache.path());
    fake.bin_dir = compiler_dir.path();
    BuiltLoader built = build_loader("gcc", repo());
    CHECK_EQ(json_dumps(Json(built.receipt["output"]["sidecars"]), DumpOptions::compact(true)),
             std::string("{\"bin/libgcc_s_seh-1.dll\":\"") + sha256_hex("libgcc_s_seh-1.dll") +
                 "\",\"bin/libstdc++-6.dll\":\"" + sha256_hex("libstdc++-6.dll") +
                 "\",\"bin/libwinpthread-1.dll\":\"" + sha256_hex("libwinpthread-1.dll") + "\"}");
    for (auto it = built.receipt["output"]["sidecars"].begin(); it != built.receipt["output"]["sidecars"].end(); ++it) {
        CHECK(fs::is_regular_file(built.path.parent_path().parent_path() / path_from_utf8(it.key())));
    }
}
#endif

TEST(player_build_and_reuse) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltPlayer built = build_player("nd2", "gcc", repo());
    fs::path worker = cache.path() / "gcc" / "player-nd2" / (is_windows() ? "worker.exe" : "worker");
    CHECK_EQ(built.command.size(), static_cast<std::size_t>(1));
    CHECK_EQ(fs::path(built.command[0]).filename(), worker.filename());
    CHECK_EQ(built.receipt["kind"], Json("player"));
    CHECK_EQ(built.receipt["output"]["sha256"].get<std::string>(), sha256_file(worker));
    CHECK_EQ(built.receipt["flags"]["player"], Json("nd2"));
    CHECK_EQ(built.receipt["flags"]["compile"], built.receipt["identity"]["flags"]);
    CHECK_EQ(fake.calls, 1);
    const std::vector<std::string>& compile = fake.commands[0];
    CHECK_EQ(compile[0], std::string("/tool/bin/g++"));
    bool nd2 = false, worker_source = false, fpm = false;
    for (const std::string& argument : compile) {
        nd2 = nd2 || argument == "-DPLAYER_ND2";
        worker_source = worker_source || (argument.size() > 10 && argument.compare(argument.size() - 10, 10, "player.cpp") == 0);
        fpm = fpm || argument.find("vendor/fpm/include") != std::string::npos ||
              argument.find("vendor\\fpm\\include") != std::string::npos;
    }
    CHECK(nd2 && worker_source && fpm);

    BuiltPlayer again = build_player("nd2", "gcc", repo());
    CHECK_EQ(fake.calls, 1);
    CHECK_EQ(again.receipt, built.receipt);
    build_player("normal_dist", "gcc", repo());
    CHECK_EQ(fake.calls, 2);
    bool plain = true;
    for (const std::string& argument : fake.commands[1]) plain = plain && argument != "-DPLAYER_ND2";
    CHECK(plain);

    write_bytes(worker, "tampered");
    build_player("nd2", "gcc", repo());
    CHECK_EQ(fake.calls, 3);
    CHECK_EQ(read_json(cache.path() / "gcc" / "player-nd2" / "build-receipt.json")["output"]["sha256"].get<std::string>(),
             sha256_file(worker));
}

TEST(wasm_players_run_through_node) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltPlayer built = build_player("normal_dist", "wasm", repo());
    CHECK_EQ(built.command.size(), static_cast<std::size_t>(2));
    CHECK_EQ(built.command[0], std::string("/usr/bin/node"));
    CHECK_EQ(fs::path(built.command[1]).filename(), fs::path("worker.js"));
    CHECK_EQ(built.receipt["identity"]["node"], Json("v20.0.0"));
    CHECK_EQ(json_dumps(Json(built.receipt["output"]["sidecars"]), DumpOptions::compact()),
             std::string("{\"worker.wasm\":\"") + sha256_hex("wasm") + "\"}");
}

namespace {

std::string keys_of(const Json& object) {
    std::string out;
    for (auto it = object.begin(); it != object.end(); ++it) out += it.key() + ",";
    return out;
}

// The configure and build flags of a loader as the recipe writes them (machine-specific prefixes replaced),
// for the fake toolchain: the compiler-dependent part is given.
std::vector<std::string> loader_flags(const std::vector<std::string>& compiler_part) {
    std::vector<std::string> flags = {"-S", "<repo>/src/vendor/digitalcurling", "-B", "<build>/cmake", "-G", "Ninja"};
    flags.insert(flags.end(), compiler_part.begin(), compiler_part.end());
    for (const char* flag :
         {"-DBOX2D_BUILD_TESTBED=OFF", "-DBOX2D_BUILD_UNIT_TESTS=OFF", "-DDIGITALCURLING_BUILD_PLAYERS=OFF",
          "-DDIGITALCURLING_BUILD_SIMULATORS=ON", "-DDIGITALCURLING_BUILD_SIMULATOR_FCV1=ON",
          "-DDIGITALCURLING_BUILD_SIMULATOR_FCV1PLUS=ON", "-DDIGITALCURLING_BUILD_PLUGIN_LOADER=ON",
          "-DDIGITALCURLING_PLUGIN_LOADER_SHARED=ON", "-DDIGITALCURLING_BUNDLE_PLUGINS=ON",
          "-DDIGITALCURLING_BUILD_TEST=OFF", "-DDIGITALCURLING_BUILD_DOCS=OFF",
          "-DFETCHCONTENT_SOURCE_DIR_FPM=<repo>/src/vendor/fpm",
          "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<repo>/src/vendor/nlohmann-json",
          "-DFETCHCONTENT_SOURCE_DIR_BOX2D=<build>/inputs/box2d-patched",
          "-DFETCHCONTENT_SOURCE_DIR_XXHASH=<repo>/src/vendor/xxhash", "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
          "--build", "<build>/cmake", "--config", "Release", "--target", "digitalcurling_plugin_loader",
          "--parallel"}) {
        flags.push_back(flag);
    }
    return flags;
}

}  // namespace

// Compilers without a variant keep the receipt and the identity (and so the caches) of earlier versions: the
// fields and the exact command lines of the recipe, with no `variant` / `flags_extra` anywhere.
static void check_plain_compiler(const std::string& id) {
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    std::vector<std::string> compiler_part = {"-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=/tool/bin/g++",
                                              "-DCMAKE_C_COMPILER=/tool/bin/gcc", "-DCMAKE_CXX_STANDARD=17"};
#ifdef _WIN32
    compiler_part.push_back("-DCMAKE_CXX_STANDARD_REQUIRED=ON");
    // The reproducibility flags of a Windows build (-ffile-prefix-map names the root; the identity writes it as <repo>).
    compiler_part.push_back("-DCMAKE_CXX_FLAGS_INIT=-ffile-prefix-map=<repo>=.");
    compiler_part.push_back("-DCMAKE_C_FLAGS_INIT=-ffile-prefix-map=<repo>=.");
    for (const char* kind : {"SHARED", "MODULE", "EXE"}) {
        compiler_part.push_back(std::string("-DCMAKE_") + kind + "_LINKER_FLAGS_INIT=-Wl,--no-insert-timestamp");
    }
#else
    compiler_part.push_back("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON");
    compiler_part.push_back("-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations");
#endif
    BuiltLoader built = build_loader(id, repo());
    const Json& receipt = built.receipt;
    CHECK_EQ(keys_of(receipt), std::string("schema,kind,compiler,flags,sources,output,host,built_at,identity,"));
    CHECK_EQ(keys_of(receipt["compiler"]), std::string("id,executable,version,target,"));
    CHECK_EQ(receipt["compiler"]["id"], Json(id));
    CHECK_EQ(keys_of(receipt["flags"]), std::string("generator,cmake,configure,build,"));
    CHECK_EQ(keys_of(receipt["identity"]), std::string("recipe,kind,sources,compiler,cmake,generator,flags,"));
    CHECK_EQ(keys_of(receipt["identity"]["compiler"]), std::string("id,version,target,"));
    CHECK_EQ(receipt["identity"]["compiler"]["id"], Json(id));
    CHECK_EQ(json_dumps(receipt["identity"]["flags"], DumpOptions::compact()),
             json_dumps(Json(loader_flags(compiler_part)), DumpOptions::compact()));
    CHECK_EQ(fake.probed[0], "loader/" + id);
    for (const std::string& compiler : fake.run_compilers) CHECK_EQ(compiler, id);
    CHECK_EQ(keys_of(receipt["host"]), std::string("system,machine,platform,python,"));
    CHECK_EQ(receipt["host"]["platform"], Json(platform_key()));

    BuiltPlayer player = build_player("nd2", id, repo());
    CHECK_EQ(keys_of(player.receipt), std::string("schema,kind,compiler,flags,sources,output,host,built_at,identity,"));
    CHECK_EQ(keys_of(player.receipt["compiler"]), std::string("id,executable,version,target,"));
    CHECK_EQ(keys_of(player.receipt["identity"]), std::string("recipe,kind,player,sources,compiler,flags,"));
    CHECK_EQ(keys_of(player.receipt["identity"]["compiler"]), std::string("id,version,target,"));
#ifdef _WIN32
    CHECK_EQ(json_dumps(player.receipt["identity"]["flags"], DumpOptions::compact()),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-DPLAYER_ND2\","
                         "\"-static\"]"));
#else
    CHECK_EQ(json_dumps(player.receipt["identity"]["flags"], DumpOptions::compact()),
             std::string("[\"-std=c++17\",\"-O3\",\"-DNDEBUG\",\"-Wno-deprecated-declarations\",\"-DPLAYER_ND2\","
                         "\"-static-libstdc++\",\"-static-libgcc\"]"));
#endif
}

TEST(a_plain_compiler_keeps_the_receipt_and_identity_format) {
    check_plain_compiler("gcc");
    check_plain_compiler("clang");
}

TEST(the_receipt_of_a_variant_names_the_base_compiler_and_the_extra_flags) {
    if (std::string(platform_key()) != "linux-arm64") return;  // refused elsewhere (test_platform.cpp)
    testing::TempDir cache;
    FakeToolchain fake(cache.path());
    BuiltLoader built = build_loader("gcc-nofma", repo());
    CHECK_EQ(fake.probed[0], std::string("loader/gcc"));  // the build script knows the base compiler only
    for (const std::string& compiler : fake.run_compilers) CHECK_EQ(compiler, std::string("gcc"));
    CHECK_EQ(built.path, cache.path() / "gcc-nofma" / "loader" / "bin" / library_name());
    CHECK_EQ(built.receipt["compiler"]["id"], Json("gcc"));
    CHECK_EQ(built.receipt["compiler"]["variant"], Json("gcc-nofma"));
    CHECK_EQ(json_dumps(built.receipt["compiler"]["flags_extra"], DumpOptions::compact()), std::string("[\"-ffp-contract=off\"]"));
    CHECK_EQ(built.receipt["identity"]["compiler"]["id"], Json("gcc-nofma"));
    CHECK_EQ(built.receipt["host"]["platform"], Json("linux-arm64"));
    CHECK_EQ(built.receipt["host"]["machine"], Json("aarch64"));
    bool c_flags = false, cxx_flags = false;
    for (const Json& flag : built.receipt["flags"]["configure"]) {
        c_flags = c_flags || flag == Json("-DCMAKE_C_FLAGS=-ffp-contract=off");
        cxx_flags = cxx_flags || flag == Json("-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -ffp-contract=off");
    }
    CHECK(c_flags && cxx_flags);
    CHECK_EQ(read_json(cache.path() / "gcc-nofma" / "loader" / "build-receipt.json"), built.receipt);

    // Its own cache; the plain compiler's identity and flags do not change, and a second run reuses it.
    BuiltLoader plain = build_loader("gcc", repo());
    CHECK_EQ(fake.calls, 4);
    CHECK(plain.path != built.path);
    CHECK(plain.receipt["identity"] != built.receipt["identity"]);
    CHECK(!plain.receipt["compiler"].contains("variant"));
    for (const Json& flag : plain.receipt["flags"]["configure"]) {
        CHECK(flag != Json("-DCMAKE_C_FLAGS=-ffp-contract=off"));
        CHECK(flag != Json("-DCMAKE_CXX_FLAGS=-Wno-deprecated-declarations -ffp-contract=off"));
    }
    BuiltLoader again = build_loader("gcc-nofma", repo());
    CHECK_EQ(fake.calls, 4);
    CHECK_EQ(again.receipt, built.receipt);

    BuiltPlayer player = build_player("nd2", "clang-nofma", repo());
    CHECK_EQ(player.receipt["compiler"]["id"], Json("clang"));
    CHECK_EQ(player.receipt["compiler"]["variant"], Json("clang-nofma"));
    CHECK_EQ(player.receipt["identity"]["compiler"]["id"], Json("clang-nofma"));
    CHECK_EQ(fake.run_compilers.back(), std::string("clang"));
    CHECK_EQ(player.receipt["flags"]["compile"].back(), Json("-ffp-contract=off"));
    const std::vector<std::string>& compile = fake.commands.back();
    CHECK(std::find(compile.begin(), compile.end(), "-ffp-contract=off") != compile.end());
    CHECK(std::find(compile.begin(), compile.end(), "-DPLAYER_ND2") < std::find(compile.begin(), compile.end(), "-ffp-contract=off"));
    CHECK(fs::is_regular_file(cache.path() / "clang-nofma" / "player-nd2" / "worker"));
}

int main() { return testing::run_all_tests(); }