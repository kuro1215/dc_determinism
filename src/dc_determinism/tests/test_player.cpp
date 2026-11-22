// The player and benchmark building blocks: PDET decoding and comparison, the manifest checks of
// player binaries (against a synthetic repository root), the benchmark channel and statistics.
// Then `run player`, `capture player` and `compare player` against the synthetic player tool_fake_player
// (no Python needed).
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/platform.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "player/binaries.hpp"
#include "player/pdet.hpp"
#include "player/result.hpp"
#include "sim/bench.hpp"
#include "sim/provenance.hpp"
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

void put32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

void set32(std::string& data, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) data[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
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

std::string write_file(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return path_text(path);
}

}  // namespace

TEST(pdet_decodes_a_valid_output) {
    PdetRecord record = decode_pdet(pdet());
    CHECK_EQ(record.player, 1u);
    CHECK_EQ(record.blocks.size(), static_cast<std::size_t>(256));
    CHECK_EQ(record.samples, static_cast<std::int64_t>(256 * 640));
    CHECK_EQ(record.restore_probes, static_cast<std::int64_t>(256 * 6 * 3));
    CHECK_EQ(record.restore_failures, static_cast<std::int64_t>(0));
    CHECK_EQ(record.sha256, sha256_hex(pdet()));
    CHECK_EQ(decode_pdet(pdet(2, 700, false, false)).restore_failures, static_cast<std::int64_t>(256 * 6));
    CHECK_EQ(decode_pdet(pdet(2, 700)).samples, static_cast<std::int64_t>(256 * 700));
}

TEST(pdet_rejects_damaged_outputs_with_the_python_messages) {
    std::string good = pdet();
    CHECK_EQ(error_of([&] { decode_pdet(std::string()); }), std::string("invalid output magic"));
    CHECK_EQ(error_of([&] { decode_pdet("XDET0001" + good.substr(8)); }), std::string("invalid output magic"));
    CHECK_EQ(error_of([&] { decode_pdet(good.substr(0, 10)); }),
             std::string("unpack_from requires a buffer of at least 16 bytes for unpacking 8 bytes at offset 8 "
                         "(actual buffer size is 10)"));
    std::string bad_player = good;
    set32(bad_player, 8, 3);
    CHECK_EQ(error_of([&] { decode_pdet(bad_player); }), std::string("invalid player or block count"));
    std::string bad_count = good;
    set32(bad_count, 12, 255);
    CHECK_EQ(error_of([&] { decode_pdet(bad_count); }), std::string("invalid player or block count"));
    CHECK_EQ(error_of([&] { decode_pdet(good.substr(0, 20)); }),
             std::string("unpack_from requires a buffer of at least 28 bytes for unpacking 12 bytes at offset 16 "
                         "(actual buffer size is 20)"));
    std::string bad_profile = good;
    set32(bad_profile, 16, 4);
    CHECK_EQ(error_of([&] { decode_pdet(bad_profile); }), std::string("invalid block identity"));
    std::string small_n = good;
    set32(small_n, 24, 639);
    CHECK_EQ(error_of([&] { decode_pdet(small_n); }), std::string("invalid block identity"));
    CHECK_EQ(error_of([&] { decode_pdet(good.substr(0, 500)); }), std::string("truncated sample records"));
    std::size_t first_probe = 16 + 12 + 224 + 640 * 24;
    CHECK_EQ(error_of([&] { decode_pdet(good.substr(0, first_probe + 5)); }),
             std::string("unpack_from requires a buffer of at least " + std::to_string(first_probe + 16) +
                         " bytes for unpacking 16 bytes at offset " + std::to_string(first_probe) +
                         " (actual buffer size is " + std::to_string(first_probe + 5) + ")"));
    std::string bad_probe = good;
    set32(bad_probe, first_probe, 9);
    CHECK_EQ(error_of([&] { decode_pdet(bad_probe); }), std::string("invalid restore probe"));
    std::string bad_flag = good;
    set32(bad_flag, first_probe + 8, 2);
    CHECK_EQ(error_of([&] { decode_pdet(bad_flag); }), std::string("invalid restore probe"));
    CHECK_EQ(error_of([&] { decode_pdet(good + std::string(1, '\0')); }),
             std::string("trailing bytes or incomplete seed/profile coverage"));
    std::string duplicate = good;
    std::size_t block1 = 16 + 12 + 224 + 640 * 24 + 96;
    set32(duplicate, block1 + 4, 0);  // block 1 gets the identity of block 0
    CHECK_EQ(error_of([&] { decode_pdet(duplicate); }), std::string("invalid block identity"));
}

TEST(pdet_comparison) {
    PdetRecord left = decode_pdet(pdet());
    Json same = compare_pdet(left, decode_pdet(pdet()));
    CHECK_EQ(same["different_samples"].get<std::int64_t>(), static_cast<std::int64_t>(0));
    CHECK(same["first_difference"].is_null());
    CHECK_EQ(json_dumps(same["different_fields"]), std::string("[0, 0, 0]"));
    CHECK_EQ(json_dumps(same["max_absolute_difference"]), std::string("[0.0, 0.0, 0.0]"));
    CHECK_EQ(json_dumps(same["different_percent"]), std::string("0.0"));
    CHECK_EQ(same["profiles"].size(), static_cast<std::size_t>(4));
    CHECK_EQ(same["profiles"][2]["samples"].get<std::int64_t>(), static_cast<std::int64_t>(64 * 640));

    Json changed = compare_pdet(left, decode_pdet(pdet(1, 640, true)));
    CHECK_EQ(changed["different_samples"].get<std::int64_t>(), static_cast<std::int64_t>(1));
    CHECK_EQ(json_dumps(changed["different_fields"]), std::string("[1, 0, 0]"));
    CHECK_EQ(json_dumps(changed["max_absolute_difference"]), std::string("[1.0, 0.0, 0.0]"));
    CHECK_EQ(json_dumps(changed["first_difference"]),
             std::string("{\"profile\": 0, \"seed\": 0, \"index\": 0, \"left_bits\": [\"00000000\", \"00000000\", "
                         "\"00000000\"], \"right_bits\": [\"3f800000\", \"00000000\", \"00000000\"]}"));
    CHECK_EQ(json_dumps(changed["different_percent"]), std::string("0.0006103515625"));
    CHECK_EQ(changed["profiles"][0]["different_samples"].get<std::int64_t>(), static_cast<std::int64_t>(1));
    CHECK_EQ(json_dumps(changed["profiles"][0]["max_absolute_difference"]), std::string("[1.0, 0.0, 0.0]"));

    CHECK_EQ(error_of([&] { compare_pdet(left, decode_pdet(pdet(2))); }),
             std::string("cross-player sequence equality is not a determinism test"));
    CHECK_EQ(error_of([&] { compare_pdet(left, decode_pdet(pdet(1, 641))); }), std::string("sample coverage conflict"));
    std::string nonfinite = pdet();
    set32(nonfinite, 16 + 12 + 224 + 12, 0x7fc00000u);
    CHECK_EQ(error_of([&] { compare_pdet(left, decode_pdet(nonfinite)); }), std::string("non-finite output"));
    std::string inputs = pdet();
    set32(inputs, 16 + 12 + 224 + 8, 4);
    CHECK_EQ(error_of([&] { compare_pdet(left, decode_pdet(inputs)); }), std::string("input shot bits conflict"));
    std::string control = pdet();
    control[16 + 12 + 40] = 1;
    CHECK_EQ(error_of([&] { compare_pdet(left, decode_pdet(control)); }),
             std::string("input configuration or raw PRNG control conflict"));
}

namespace {

// A synthetic repository root with a manifest carrying a `players` section .
struct Root {
    testing::TempDir dir;
    fs::path manifest() const { return dir.path() / "prebuilt" / "manifest.json"; }
    fs::path wasm() const { return dir.path() / "prebuilt" / "players" / "wasm" / "nd2"; }
    fs::path native() const { return dir.path() / "prebuilt" / "players" / "native" / "nd2" / "worker.exe"; }

    Json document;

    Root() {
        write_file(wasm() / "worker.js", "js-code");
        write_file(wasm() / "worker.wasm", "wasm-code");
        write_file(native(), "exe-code");
#ifndef _WIN32
        fs::permissions(native(), fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add);
#endif
        document = Json::object();
        document["schema"] = "dc-determinism-prebuilt-v2";
        document["source"] = Json::object({{"path", "src/worker/worker.cpp"}, {"sha256", std::string(64, '0')}});
        document["binaries"] = Json::object();
        Json wasm_entry = Json::object({{"path", "prebuilt/players/wasm/nd2/worker.js"},
                                        {"sha256", sha256_hex("js-code")},
                                        {"compiler", "em++ 6.0.9"},
                                        {"flags", Json::array({"-O3"})},
                                        {"sidecars", Json::object({{"worker.wasm", sha256_hex("wasm-code")}})}});
        Json native_entry = Json::object({{"path", "prebuilt/players/native/nd2/worker.exe"},
                                          {"sha256", sha256_hex("exe-code")},
                                          {"compiler", "g++"},
                                          {"flags", Json::array({"-O3"})}});
        Json binaries = Json::object();
        binaries["wasm/nd2"] = wasm_entry;
        binaries[std::string(platform_key()) + "-gcc/nd2"] = native_entry;
        document["players"] = Json::object({{"source", Json::object({{"worker_sha256", std::string(64, '1')}})},
                                            {"binaries", binaries}});
        save();
    }

    void save() { write_file(manifest(), json_dumps(document)); }
    Json& entry(const std::string& key) { return document["players"]["binaries"][key]; }
};

}  // namespace

TEST(player_binary_keys) {
    CHECK_EQ(player_binary_key("nd2", "wasm"), std::string("wasm/nd2"));
    CHECK_EQ(player_binary_key("normal_dist", "gcc"), std::string(platform_key()) + "-gcc/normal_dist");
    if (!is_windows()) CHECK(!error_of([] { player_binary_key("nd2", "msvc"); }).empty());
}

TEST(player_binaries_are_verified_against_the_manifest) {
    Root root;
    Json wasm = verify_player_binary("wasm/nd2", root.dir.path());
    CHECK_EQ(wasm["sha256"].get<std::string>(), sha256_hex("js-code"));
    CHECK_EQ(json_dumps(wasm["sidecars"]), std::string("{\"worker.wasm\": \"") + sha256_hex("wasm-code") + "\"}");
    CHECK_EQ(wasm["compiler"].get<std::string>(), std::string("em++ 6.0.9"));
    CHECK(wasm["sidecar_paths"]["worker.wasm"].get<std::string>().find("worker.wasm") != std::string::npos);
    std::string native_key = std::string(platform_key()) + "-gcc/nd2";
    Json native = verify_player_binary(native_key, root.dir.path());
    CHECK_EQ(json_dumps(native["flags"]), std::string("[\"-O3\"]"));
    recheck_binary(native);
    recheck_binary(wasm);

    // Tampering.
    write_file(root.wasm() / "worker.wasm", "other");
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("sidecar hash mismatch") !=
          std::string::npos);
    CHECK_EQ(error_of([&] { recheck_binary(wasm); }), std::string("player binary changed between repetitions"));
    fs::remove(root.wasm() / "worker.wasm");
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("player sidecar not found") == 0);
    write_file(root.wasm() / "worker.wasm", "wasm-code");
    write_file(root.wasm() / "worker.js", "changed");
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("player hash mismatch") == 0);
    fs::remove(root.wasm() / "worker.js");
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("player binary not found") == 0);
}

TEST(player_manifest_errors) {
    Root root;
    CHECK(error_of([&] { verify_player_binary("wasm/normal_dist", root.dir.path()); })
              .find("no prebuilt player binary 'wasm/normal_dist' in the manifest") == 0);
    root.entry("wasm/nd2")["path"] = "../outside.js";
    root.save();
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("unsafe player path") == 0);
    root.entry("wasm/nd2")["path"] = "prebuilt/players/wasm/nd2/worker.js";
    root.entry("wasm/nd2")["sidecars"] = Json::object({{"../x.wasm", std::string(64, '0')}});
    root.save();
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("unsafe sidecar name") == 0);
    root.document["players"].erase("binaries");
    root.save();
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("no `players` section") != std::string::npos);
    root.document.erase("players");
    root.save();
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("no `players` section") != std::string::npos);
    fs::remove(root.manifest());
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("worker manifest not found") == 0);
}

TEST(player_sidecars_with_repository_relative_paths) {
    Root root;
    std::string relative = "prebuilt/players/wasm/nd2/worker.wasm";
    auto set_sidecars = [&](const Json& value) {
        root.entry("wasm/nd2")["sidecars"] = value;
        root.save();
    };
    set_sidecars(Json::object({{"worker.wasm", Json::object({{"path", relative}, {"sha256", sha256_hex("wasm-code")}})}}));
    Json info = verify_player_binary("wasm/nd2", root.dir.path());
    CHECK_EQ(json_dumps(info["sidecars"]), std::string("{\"worker.wasm\": \"") + sha256_hex("wasm-code") + "\"}");
    CHECK(!describe_explicit_player(root.wasm() / "worker.js", "wasm/nd2", root.dir.path())["manifest_entry"].is_null());
    for (const char* bad : {"../worker.wasm", "outside/worker.wasm", "prebuilt/../x.wasm"}) {
        set_sidecars(Json::object({{"worker.wasm", Json::object({{"path", bad}, {"sha256", sha256_hex("wasm-code")}})}}));
        CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("unsafe sidecar path") == 0);
    }
    set_sidecars(Json::object({{"worker.wasm", Json::object({{"path", relative}})}}));
    CHECK(error_of([&] { verify_player_binary("wasm/nd2", root.dir.path()); }).find("invalid sidecar hash") == 0);
}

TEST(explicit_players_match_the_manifest_only_when_identical) {
    Root root;
    Json info = describe_explicit_player(root.wasm() / "worker.js", "wasm/nd2", root.dir.path());
    CHECK_EQ(json_dumps(info["sidecars"]), std::string("{\"worker.wasm\": \"") + sha256_hex("wasm-code") + "\"}");
    CHECK(!info["manifest_entry"].is_null());
    CHECK(info.contains("compiler") && !info.contains("sidecar_paths"));
    write_file(root.wasm() / "worker.js", "patched");
    info = describe_explicit_player(root.wasm() / "worker.js", "wasm/nd2", root.dir.path());
    CHECK(info["manifest_entry"].is_null());
    CHECK_EQ(info["sha256"].get<std::string>(), sha256_hex("patched"));
    CHECK(error_of([&] { describe_explicit_player(root.wasm() / "nope.js", "wasm/nd2", root.dir.path()); })
              .find("not found") != std::string::npos);
}

TEST(player_command_hook_is_a_json_array_of_strings) {
    for (const char* text : {"{", "[]", "[\"a\", 1]", "\"x\"", "nope"}) {
        CHECK(!error_of([&] { parse_player_command(text); }).empty());
    }
    std::vector<std::string> parsed = parse_player_command("[\"a\", \"b\"]");
    CHECK_EQ(parsed.size(), static_cast<std::size_t>(2));
    CHECK_EQ(parsed[1], std::string("b"));
    // With a command hook nothing is verified.
    PlayerSelection selection = resolve_player("nd2", "gcc", "[\"x\", \"y\"]");
    CHECK_EQ(selection.command.size(), static_cast<std::size_t>(2));
    CHECK(selection.binary["path"].is_null() && selection.binary["sha256"].is_null());
    CHECK_EQ(selection.binary["key"].get<std::string>(), std::string(platform_key()) + "-gcc/nd2");

}

TEST(find_on_path_looks_through_path) {
    testing::TempDir dir;
    std::string name = is_windows() ? "dc_determinism_probe_tool.exe" : "dc_determinism_probe_tool";
    write_file(dir.path() / name, "x");
#ifndef _WIN32
    fs::permissions(dir.path() / name, fs::perms::owner_exec, fs::perm_options::add);
    setenv("PATH", path_text(dir.path()).c_str(), 1);
#else
    _putenv_s("PATH", path_text(dir.path()).c_str());
#endif
    std::optional<fs::path> found = find_on_path("dc_determinism_probe_tool");
    CHECK(found.has_value());
    if (found) {  // the file system (and PATHEXT) decide the case on Windows
        std::string actual = found->filename().string();
        for (char& c : actual) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        CHECK_EQ(actual, name);
    }
    CHECK(!find_on_path("dc_determinism_no_such_tool").has_value());
}

TEST(benchmark_lines_and_order) {
    CHECK_EQ(parse_bench_line("B 3 fcv1 12345\r\n", 3, "fcv1"), static_cast<std::int64_t>(12345));
    CHECK(error_of([] { parse_bench_line("ERR 3 boom", 3, "fcv1"); }).find("worker reported: ERR 3 boom") == 0);
    for (const char* bad : {"B 4 fcv1 1", "B 3 fcv1plus 1", "B 3 fcv1 -1", "B 3 fcv1 x", "B 3 fcv1", "x 3 fcv1 1",
                            "B 3 fcv1 1 2", ""}) {
        CHECK(error_of([&] { parse_bench_line(bad, 3, "fcv1"); })
                  .find("unexpected worker output for benchmark request 3 fcv1: ") == 0);
    }
    std::vector<std::string> engines = {"fcv1", "fcv1plus"};
    CHECK(engine_order(engines, 0) == engines);
    CHECK(engine_order(engines, 1) == (std::vector<std::string>{"fcv1plus", "fcv1"}));
    CHECK(engine_order(engines, 4) == engines);
}

TEST(benchmark_statistics_keep_the_python_number_types) {
    auto ints = [](std::initializer_list<std::int64_t> values) {
        std::vector<Json> out;
        for (std::int64_t value : values) out.push_back(value);
        return out;
    };
    auto floats = [](std::initializer_list<double> values) {
        std::vector<Json> out;
        for (double value : values) out.push_back(value);
        return out;
    };
    CHECK_EQ(json_dumps(py_median(ints({5, 1, 3}))), std::string("3"));
    CHECK_EQ(json_dumps(py_median(ints({1, 2}))), std::string("1.5"));
    CHECK_EQ(json_dumps(py_median(ints({2, 4}))), std::string("3.0"));
    CHECK_EQ(json_dumps(py_median(ints({7}))), std::string("7"));
    CHECK_EQ(json_dumps(py_median(floats({3.0, 1.0, 2.0}))), std::string("2.0"));
    CHECK_EQ(json_dumps(py_median(floats({1.0, 2.5, 4.0, 9.0}))), std::string("3.25"));
    CHECK(!error_of([&] { py_median({}); }).empty());
    CHECK_EQ(json_dumps(py_sum(ints({1, 2, 3}))), std::string("6"));
    CHECK_EQ(json_dumps(py_sum(floats({0.1, 0.2, 0.3}))), std::string("0.6"));  // sum([.1, .2, .3]) == 0.6 since 3.12
    CHECK_EQ(json_dumps(py_sum(floats({1e100, 1.0, -1e100}))), std::string("1.0"));  // compensated
    CHECK_EQ(json_dumps(py_sum(floats({0.5}))), std::string("0.5"));
}

TEST(benchmark_summary) {
    Json records = Json::array();
    for (int i = 0; i < 3; ++i) {
        Json medians = Json::object();
        medians["fcv1"] = 1000 * (i + 1) + 2.5;
        medians["fcv1plus"] = static_cast<std::int64_t>(3000 * (i + 1));
        records.push_back(Json::object({{"median_ns", medians}}));
    }
    Json summary = summarize_benchmark(records, {"fcv1", "fcv1plus"});
    CHECK_EQ(json_dumps(summary["fcv1"]),
             std::string("{\"shots\": 3, \"median_of_medians_ns\": 2002.5, \"mean_of_medians_ns\": 2002.5, "
                         "\"total_of_medians_ns\": 6007.5}"));
    CHECK_EQ(json_dumps(summary["fcv1plus"]),
             std::string("{\"shots\": 3, \"median_of_medians_ns\": 6000, \"mean_of_medians_ns\": 6000.0, "
                         "\"total_of_medians_ns\": 18000}"));
}

namespace {

std::string worker_exe() {
    return path_to_utf8(executable_path().parent_path() / (is_windows() ? "tool_fake_worker.exe" : "tool_fake_worker"));
}

}  // namespace

TEST(benchmark_channel_talks_to_a_worker) {
    {
        BenchChannel channel({worker_exe()});
        CHECK_EQ(parse_bench_line(channel.request("B 0 fcv1", 30.0), 0, "fcv1"), static_cast<std::int64_t>(1000));
        CHECK_EQ(parse_bench_line(channel.request("B 4 fcv1plus", 0.0), 4, "fcv1plus"), static_cast<std::int64_t>(5000));
        std::optional<std::int64_t> code = channel.close();
        CHECK(code.has_value() && *code == 0);
    }
    {
        BenchChannel channel({worker_exe(), "--mode", "eof"});
        std::string message = error_of([&] { channel.request("B 0 fcv1", 30.0); });
        CHECK(message.find("worker ended unexpectedly while processing 'B 0 fcv1' (exit code ") == 0);
    }
    {
        BenchChannel channel({worker_exe(), "--mode", "hang"});
        auto started = std::chrono::steady_clock::now();
        std::string message = error_of([&] { channel.request("B 0 fcv1", 1.0); });
        CHECK_EQ(message, std::string("worker did not answer 'B 0 fcv1' within 1 s"));
        CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < 20.0);
    }
    CHECK(error_of([] { BenchChannel channel({"dc_determinism-no-such-program-xyz"}); }).find("cannot start worker") == 0);
}

// ---- run player / capture player / compare player, against the synthetic player ----

namespace {

struct Outcome {
    int code = 0;
    std::string error;
};

// Parses and dispatches a command line in this process (what main() does).
Outcome run_cli(const std::vector<std::string>& words) {
    Outcome out;
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    try {
        cli::ParsedArgs args = parser->parse(words);
        out.code = cli::dispatch(args);
    } catch (const cli::ParseExit& exit) {
        out.code = exit.code == 0 ? 10 : exit.code;
    } catch (const DcDeterminismError& error) {
        out.code = 2;
        out.error = error.what();
    }
    return out;
}

std::string player_exe() {
    return path_to_utf8(executable_path().parent_path() / (is_windows() ? "tool_fake_player.exe" : "tool_fake_player"));
}

// `--player-command` for the synthetic player: `tool_fake_player <options>`.
std::string hook(const std::vector<std::string>& options = {}) {
    Json command = Json::array();
    command.push_back(player_exe());
    for (const std::string& option : options) command.push_back(option);
    return json_dumps(command);
}

std::vector<std::string> join(std::vector<std::string> first, const std::vector<std::string>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// `run player` of the synthetic nd2 player.
std::vector<std::string> run_args(const fs::path& output, const std::vector<std::string>& fake = {"--id", "2"},
                                  const std::vector<std::string>& extra = {}, const std::string& samples = "640") {
    return join({"run", "player", "--player", "nd2", "--compiler", "gcc", "--samples", samples, "--output",
                 path_text(output), "--player-command", hook(fake)},
                extra);
}

std::vector<std::string> capture_args(const fs::path& output, const std::vector<std::string>& fake = {"--id", "2"},
                                      const std::vector<std::string>& extra = {}, const std::string& samples = "640") {
    return join({"capture", "player", "--player", "nd2", "--compiler", "gcc", "--samples", samples, "--output",
                 path_text(output), "--player-command", hook(fake)},
                extra);
}

std::vector<std::string> names_in(const fs::path& directory) {
    std::vector<std::string> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory)) {
        names.push_back(path_to_utf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

Json load(const fs::path& path) { return read_json(path); }

// Writes a result file with `binary` fields changed, for the classification tests.
fs::path copy_result(const fs::path& from, const fs::path& to, const std::function<void(Json&)>& edit) {
    Json document = load(from);
    edit(document);
    write_file(to, json_dumps(document));
    return to;
}

}  // namespace

TEST(run_player_writes_only_the_result_json) {
    testing::TempDir dir;
    fs::path output = dir / "nested" / "result.json";
    Outcome outcome = run_cli(run_args(output));
    CHECK_EQ(outcome.error, std::string());
    CHECK_EQ(outcome.code, 0);
    CHECK(names_in(dir.path() / "nested") == (std::vector<std::string>{"result.json"}));
    Json result = load(output);
    CHECK_EQ(result["schema"].get<std::string>(), std::string("dc-determinism-player-result-v1"));
    CHECK_EQ(result["player"].get<std::string>(), std::string("nd2"));
    CHECK_EQ(result["compiler"].get<std::string>(), std::string("gcc"));
    CHECK_EQ(result["samples_per_block"].get<std::int64_t>(), static_cast<std::int64_t>(640));
    CHECK_EQ(result["repeats"].get<std::int64_t>(), static_cast<std::int64_t>(2));
    CHECK(result["worker_sha256"].is_null());
    CHECK_EQ(result["runs"].size(), static_cast<std::size_t>(2));
    std::string digest = sha256_hex(pdet(2, 640));
    for (std::size_t i = 0; i < 2; ++i) {
        const Json& run = result["runs"][i];
        CHECK_EQ(run["index"].get<std::int64_t>(), static_cast<std::int64_t>(i + 1));
        CHECK_EQ(run["sha256"].get<std::string>(), digest);
        CHECK_EQ(run["samples"].get<std::int64_t>(), static_cast<std::int64_t>(256 * 640));
        CHECK_EQ(run["restore_failures"].get<std::int64_t>(), static_cast<std::int64_t>(0));
        CHECK_EQ(run["metadata"]["compiler"].get<std::string>(), std::string("fake"));
        CHECK(!run.contains("file"));
    }
    CHECK(result["reproducible"].get<bool>());
    CHECK_EQ(result["output_sha256"].get<std::string>(), digest);
    CHECK_EQ(result["distinct_output_sha256"].get<std::int64_t>(), static_cast<std::int64_t>(1));
    for (const char* key : {"source_revision", "binary", "runtime", "players_source", "environment"}) {
        CHECK(result.contains(key));
    }
    CHECK(!result.contains("comparisons"));

    // The result is itself valid input of the comparison and refuses to be overwritten.
    PlayerResult parsed = parse_player_result(read_file(output), "result.json");
    CHECK(parsed.reproducible);
    CHECK_EQ(parsed.output_sha256.get<std::string>(), digest);
    Outcome again = run_cli(run_args(output));
    CHECK_EQ(again.code, 2);
    CHECK(again.error.find("refusing to overwrite existing output") == 0);
    CHECK_EQ(load(output)["output_sha256"].get<std::string>(), digest);
}

TEST(run_player_reports_differing_repetitions_with_exit_1) {
    testing::TempDir dir;
    fs::path output = dir / "result.json";
    fs::path counter = dir / "counter";
    Outcome outcome = run_cli(run_args(output, {"--id", "2", "--mode", "alternate", "--counter", path_text(counter)}));
    CHECK_EQ(outcome.error, std::string());
    CHECK_EQ(outcome.code, 1);
    Json result = load(output);
    CHECK(!result["reproducible"].get<bool>());
    CHECK(result["output_sha256"].is_null());
    CHECK_EQ(result["distinct_output_sha256"].get<std::int64_t>(), static_cast<std::int64_t>(2));
    CHECK(result["runs"][0]["sha256"] != result["runs"][1]["sha256"]);
    CHECK(names_in(dir.path()) == (std::vector<std::string>{"counter", "result.json"}));

    // Three repetitions: the outputs are compared with the first one.
    fs::path three = dir / "three.json";
    fs::remove(counter);
    Outcome many = run_cli(run_args(three, {"--id", "2", "--mode", "alternate", "--counter", path_text(counter)},
                                    {"--repeats", "3"}));
    CHECK_EQ(many.code, 1);
    CHECK_EQ(load(three)["runs"].size(), static_cast<std::size_t>(3));
    CHECK_EQ(load(three)["distinct_output_sha256"].get<std::int64_t>(), static_cast<std::int64_t>(2));
}

TEST(run_player_failures_leave_no_output_and_no_temporary_directory) {
    struct Scenario {
        const char* label;
        std::vector<std::string> fake;
        std::vector<std::string> extra;
        const char* message;
    };
    const Scenario scenarios[] = {
        {"player fails", {"--id", "2", "--mode", "fail"}, {}, "player exited with code 3: simulated player failure"},
        {"garbage", {"--id", "2", "--mode", "garbage"}, {}, "invalid player output"},
        {"restore probe", {"--id", "2", "--mode", "restore-fail"}, {}, "player storage continuation probe failed"},
        {"hang", {"--id", "2", "--mode", "hang"}, {"--timeout", "1"}, "player run timed out after 1 s"},
        {"other player", {"--id", "1"}, {}, "is a different player's output"},
        {"other sample size", {"--id", "2", "--write-samples", "641"}, {}, "has a different sample size"},
        {"input conflict", {"--id", "2", "--mode", "conflict"}, {}, "cannot compare player outputs"},
    };
    for (const Scenario& scenario : scenarios) {
        testing::TempDir dir;
        fs::path counter = dir / "counter";
        std::vector<std::string> fake = scenario.fake;
        if (std::string(scenario.label) == "input conflict") {
            fake.push_back("--counter");
            fake.push_back(path_text(counter));
        }
        fs::path output = dir / "out" / "result.json";
        Outcome outcome = run_cli(run_args(output, fake, scenario.extra));
        if (outcome.code != 2 || outcome.error.find(scenario.message) == std::string::npos) {
            ::testing::report_failure(__FILE__, __LINE__,
                                      std::string(scenario.label) + ": code " + std::to_string(outcome.code) + ": " +
                                          outcome.error);
        }
        CHECK(!fs::exists(output));
        std::vector<std::string> left;
        if (fs::exists(dir.path() / "out")) left = names_in(dir.path() / "out");
        if (!left.empty()) ::testing::report_failure(__FILE__, __LINE__, std::string(scenario.label) + ": left " + left[0]);
    }
    // A command that does not exist, and the samples range.
    testing::TempDir dir;
    Json missing = Json::array();
    missing.push_back(path_text(dir / "no-such-player"));
    CHECK_EQ(run_cli(run_args(dir / "a.json")).code, 0);
    Outcome start = run_cli({"run", "player", "--player", "nd2", "--compiler", "gcc", "--output",
                             path_text(dir / "b.json"), "--player-command", json_dumps(missing)});
    CHECK_EQ(start.code, 2);
    CHECK(start.error.find("cannot start player") == 0);
    CHECK(!fs::exists(dir / "b.json"));
    Outcome range = run_cli({"run", "player", "--player", "nd2", "--compiler", "gcc", "--samples", "1000001",
                             "--output", path_text(dir / "c.json"), "--player-command", hook()});
    CHECK_EQ(range.code, 2);
    CHECK(range.error.find("--samples must be within 640..1000000") == 0);
    Outcome removed_option = run_cli(join(run_args(dir / "d.json"), {"--dc-root", path_text(dir.path())}));
    CHECK_EQ(removed_option.code, 2);  // the option is gone: an argument error
    CHECK(!fs::exists(dir / "d.json"));
    CHECK(names_in(dir.path()) == (std::vector<std::string>{"a.json"}));
}

TEST(capture_player_keeps_the_output_and_checks_the_expectation) {
    testing::TempDir dir;
    fs::path result = dir / "result.json";
    CHECK_EQ(run_cli(run_args(result)).code, 0);
    std::string digest = sha256_hex(pdet(2, 640));

    fs::path output = dir / "out" / "captured.bin";
    Outcome plain = run_cli(capture_args(output));
    CHECK_EQ(plain.error, std::string());
    CHECK_EQ(plain.code, 0);
    CHECK(names_in(dir.path() / "out") == (std::vector<std::string>{"captured.bin"}));
    CHECK_EQ(sha256_hex(read_file(output)), digest);
    CHECK_EQ(read_file(output), pdet(2, 640));

    // Never overwritten.
    Outcome again = run_cli(capture_args(output));
    CHECK_EQ(again.code, 2);
    CHECK(again.error.find("refusing to overwrite existing output") == 0);
    CHECK_EQ(sha256_hex(read_file(output)), digest);

    // --expect equal: exit 0.
    fs::path equal = dir / "equal.bin";
    CHECK_EQ(run_cli(capture_args(equal, {"--id", "2"}, {"--expect", path_text(result)})).code, 0);
    CHECK_EQ(sha256_hex(read_file(equal)), digest);

    // --expect differing: exit 1, the .bin is still written.
    fs::path counter = dir / "counter";
    write_file(counter, "1");
    fs::path different = dir / "different.bin";
    Outcome mismatch = run_cli(capture_args(
        different, {"--id", "2", "--mode", "alternate", "--counter", path_text(counter)}, {"--expect", path_text(result)}));
    CHECK_EQ(mismatch.error, std::string());
    CHECK_EQ(mismatch.code, 1);
    CHECK(fs::exists(different));
    CHECK(sha256_hex(read_file(different)) != digest);
}

TEST(capture_player_validates_the_expectation_before_starting_the_player) {
    testing::TempDir dir;
    fs::path result = dir / "result.json";
    CHECK_EQ(run_cli(run_args(result)).code, 0);
    fs::path log = dir / "calls.log";
    std::vector<std::string> logging = {"--id", "2", "--log", path_text(log)};

    // Another sample size, another player, another compiler.
    fs::path other_samples = dir / "other-samples.bin";
    Outcome samples = run_cli(capture_args(other_samples, logging, {"--expect", path_text(result)}, "700"));
    CHECK_EQ(samples.code, 2);
    CHECK(samples.error.find("--expect result is for nd2 / gcc / 640 samples per block, not nd2 / gcc / 700") == 0);
    Outcome compiler = run_cli({"capture", "player", "--player", "nd2", "--compiler", "clang", "--samples", "640",
                                "--output", path_text(dir / "clang.bin"), "--expect", path_text(result),
                                "--player-command", hook(logging)});
    CHECK_EQ(compiler.code, 2);
    CHECK(compiler.error.find("--expect result is for nd2 / gcc") == 0);
    // A result that is not reproducible.
    fs::path unstable = dir / "unstable.json";
    fs::path counter = dir / "counter";
    CHECK_EQ(run_cli(run_args(unstable, {"--id", "2", "--mode", "alternate", "--counter", path_text(counter)})).code, 1);
    Outcome not_reproducible = run_cli(capture_args(dir / "unstable.bin", logging, {"--expect", path_text(unstable)}));
    CHECK_EQ(not_reproducible.code, 2);
    CHECK(not_reproducible.error.find("is not reproducible") != std::string::npos);
    // A damaged result and a missing one.
    fs::path damaged = copy_result(result, dir / "damaged.json", [](Json& document) {
        document["output_sha256"] = std::string(64, 'f');
    });
    Outcome inconsistent = run_cli(capture_args(dir / "damaged.bin", logging, {"--expect", path_text(damaged)}));
    CHECK_EQ(inconsistent.code, 2);
    CHECK(inconsistent.error.find("invalid player result") == 0);
    Outcome missing = run_cli(capture_args(dir / "missing.bin", logging, {"--expect", path_text(dir / "nope.json")}));
    CHECK_EQ(missing.code, 2);
    CHECK(missing.error.find("cannot read") == 0);

    // None of these started the player or wrote an output.
    CHECK(!fs::exists(log));
    for (const char* name : {"other-samples.bin", "clang.bin", "unstable.bin", "damaged.bin", "missing.bin"}) {
        CHECK(!fs::exists(dir / name));
    }
    for (const std::string& name : names_in(dir.path())) CHECK(name.find(".tmp") == std::string::npos);
}

TEST(capture_player_failures_leave_no_output_and_no_temporary_directory) {
    struct Scenario {
        const char* label;
        std::vector<std::string> fake;
        std::vector<std::string> extra;
        const char* message;
    };
    const Scenario scenarios[] = {
        {"player fails", {"--id", "2", "--mode", "fail"}, {}, "player exited with code 3"},
        {"garbage", {"--id", "2", "--mode", "garbage"}, {}, "invalid player output"},
        {"restore probe", {"--id", "2", "--mode", "restore-fail"}, {}, "player storage continuation probe failed"},
        {"hang", {"--id", "2", "--mode", "hang"}, {"--timeout", "1"}, "player run timed out after 1 s"},
        {"other player", {"--id", "1"}, {}, "is a different player's output"},
        {"other sample size", {"--id", "2", "--write-samples", "641"}, {}, "has a different sample size"},
    };
    for (const Scenario& scenario : scenarios) {
        testing::TempDir dir;
        fs::path output = dir / "out" / "captured.bin";
        Outcome outcome = run_cli(capture_args(output, scenario.fake, scenario.extra));
        if (outcome.code != 2 || outcome.error.find(scenario.message) == std::string::npos) {
            ::testing::report_failure(__FILE__, __LINE__,
                                      std::string(scenario.label) + ": code " + std::to_string(outcome.code) + ": " +
                                          outcome.error);
        }
        CHECK(!fs::exists(output));
        std::vector<std::string> left;
        if (fs::exists(dir.path() / "out")) left = names_in(dir.path() / "out");
        if (!left.empty()) ::testing::report_failure(__FILE__, __LINE__, std::string(scenario.label) + ": left " + left[0]);
    }
}

TEST(compare_player_on_outputs_keeps_its_metrics) {
    testing::TempDir dir;
    fs::path good = dir / "good.bin";
    fs::path changed = dir / "changed.bin";
    CHECK_EQ(run_cli(capture_args(good)).code, 0);
    CHECK_EQ(run_cli(capture_args(changed, {"--id", "2", "--mode", "changed"})).code, 0);

    fs::path same = dir / "same.json";
    Outcome equal = run_cli({"compare", "player", path_text(good), path_text(good), "--output", path_text(same)});
    CHECK_EQ(equal.error, std::string());
    CHECK_EQ(equal.code, 0);
    Json report = load(same);
    CHECK_EQ(report["schema"].get<std::string>(), std::string("dc-determinism-player-comparison-v2"));
    CHECK(report["equal"].get<bool>());
    CHECK_EQ(report["inputs"][0]["sha256"].get<std::string>(), sha256_hex(pdet(2, 640)));
    CHECK(!testing::has_path_key(report["inputs"]));
    CHECK_EQ(report["comparison"]["different_samples"].get<std::int64_t>(), static_cast<std::int64_t>(0));

    fs::path diff = dir / "diff.json";
    Outcome unequal = run_cli({"compare", "player", path_text(good), path_text(changed), "--output", path_text(diff)});
    CHECK_EQ(unequal.code, 1);
    Json other = load(diff);
    CHECK(!other["equal"].get<bool>());
    CHECK_EQ(other["comparison"]["different_samples"].get<std::int64_t>(), static_cast<std::int64_t>(1));
    CHECK_EQ(json_dumps(other["comparison"]["different_fields"]), std::string("[1, 0, 0]"));

    // The output is never overwritten; other players cannot be compared; damaged inputs are rejected.
    Outcome overwrite = run_cli({"compare", "player", path_text(good), path_text(good), "--output", path_text(diff)});
    CHECK_EQ(overwrite.code, 2);
    write_file(dir / "normal_dist.bin", pdet(1, 640));
    Outcome players = run_cli({"compare", "player", path_text(good), path_text(dir / "normal_dist.bin"), "--output",
                               path_text(dir / "p.json")});
    CHECK_EQ(players.code, 2);
    CHECK(players.error.find("cannot compare player outputs: cross-player") == 0);
    write_file(dir / "garbage.bin", "garbage");
    Outcome garbage = run_cli({"compare", "player", path_text(good), path_text(dir / "garbage.bin"), "--output",
                               path_text(dir / "g.json")});
    CHECK_EQ(garbage.code, 2);
    CHECK(garbage.error.find("garbage.bin is neither a player output") != std::string::npos);
    write_file(dir / "short.bin", pdet(2, 640).substr(0, 100));
    Outcome truncated = run_cli({"compare", "player", path_text(good), path_text(dir / "short.bin"), "--output",
                                 path_text(dir / "s.json")});
    CHECK_EQ(truncated.code, 2);
    CHECK(truncated.error.find("invalid player output") == 0);
    CHECK(!fs::exists(dir / "g.json") && !fs::exists(dir / "s.json") && !fs::exists(dir / "p.json"));
}

TEST(compare_player_on_results) {
    testing::TempDir dir;
    fs::path a = dir / "a.json";
    fs::path b = dir / "b.json";
    fs::path c = dir / "c.json";
    CHECK_EQ(run_cli(run_args(a)).code, 0);
    CHECK_EQ(run_cli(run_args(b)).code, 0);
    CHECK_EQ(run_cli(run_args(c, {"--id", "2", "--mode", "changed"})).code, 0);
    auto compare = [&](const fs::path& left, const fs::path& right, const fs::path& out) {
        return run_cli({"compare", "player", path_text(left), path_text(right), "--output", path_text(out)});
    };

    // Equal pair: cross-binary because the synthetic player has no recorded binary hash.
    Outcome equal = compare(a, b, dir / "ab.json");
    CHECK_EQ(equal.error, std::string());
    CHECK_EQ(equal.code, 0);
    Json report = load(dir / "ab.json");
    CHECK_EQ(report["schema"].get<std::string>(), std::string("dc-determinism-player-result-comparison-v2"));
    CHECK(report["equal"].get<bool>());
    CHECK(!testing::has_path_key(report["inputs"]));
    CHECK_EQ(report["classification"].get<std::string>(), std::string("cross-binary"));
    CHECK_EQ(report["inputs"].size(), static_cast<std::size_t>(2));
    CHECK_EQ(report["inputs"][0]["sha256"].get<std::string>(), sha256_hex(read_file(a)));
    CHECK_EQ(report["inputs"][1]["player"].get<std::string>(), std::string("nd2"));
    CHECK_EQ(report["inputs"][1]["samples_per_block"].get<std::int64_t>(), static_cast<std::int64_t>(640));
    CHECK(report["inputs"][0]["worker_sha256"].is_null());
    CHECK_EQ(report["compatible"]["player"].get<std::string>(), std::string("nd2"));
    CHECK(report["reproducible"]["left"].get<bool>() && report["reproducible"]["right"].get<bool>());
    CHECK_EQ(report["output_sha256"]["left"].get<std::string>(), sha256_hex(pdet(2, 640)));
    CHECK(!report.contains("comparison"));

    // A different output: exit 1.
    Outcome different = compare(a, c, dir / "ac.json");
    CHECK_EQ(different.code, 1);
    CHECK(!load(dir / "ac.json")["equal"].get<bool>());

    // Same binary: the same worker hash and sidecars.
    std::string worker(64, '1');
    fs::path same_a = copy_result(a, dir / "same-a.json", [&](Json& d) { d["worker_sha256"] = worker; });
    fs::path same_b = copy_result(b, dir / "same-b.json", [&](Json& d) { d["worker_sha256"] = worker; });
    CHECK_EQ(compare(same_a, same_b, dir / "same.json").code, 0);
    CHECK_EQ(load(dir / "same.json")["classification"].get<std::string>(), std::string("same-binary"));
    fs::path side = copy_result(b, dir / "side.json", [&](Json& d) {
        d["worker_sha256"] = worker;
        d["binary"]["sidecars"] = Json::object({{"worker.wasm", std::string(64, '2')}});
    });
    CHECK_EQ(compare(same_a, side, dir / "side-report.json").code, 0);
    CHECK_EQ(load(dir / "side-report.json")["classification"].get<std::string>(), std::string("cross-binary"));
    fs::path other_worker = copy_result(b, dir / "other-worker.json", [&](Json& d) { d["worker_sha256"] = std::string(64, '3'); });
    CHECK_EQ(compare(same_a, other_worker, dir / "worker-report.json").code, 0);
    CHECK_EQ(load(dir / "worker-report.json")["classification"].get<std::string>(), std::string("cross-binary"));

    // A side that is not reproducible: exit 1 even with a matching first run.
    fs::path unstable = dir / "unstable.json";
    CHECK_EQ(run_cli(run_args(unstable, {"--id", "2", "--mode", "alternate", "--counter", path_text(dir / "counter")}))
                 .code, 1);
    Outcome flaky = compare(a, unstable, dir / "flaky.json");
    CHECK_EQ(flaky.code, 1);
    Json flaky_report = load(dir / "flaky.json");
    CHECK(!flaky_report["equal"].get<bool>());
    CHECK(flaky_report["reproducible"]["left"].get<bool>());
    CHECK(!flaky_report["reproducible"]["right"].get<bool>());
    CHECK(flaky_report["output_sha256"]["right"].is_null());
    CHECK_EQ(compare(unstable, unstable, dir / "both-flaky.json").code, 1);

    // Errors: another sample size or player, mixed kinds, unknown schema, damaged results.
    fs::path other_samples = copy_result(a, dir / "other-samples.json", [](Json& d) { d["samples_per_block"] = 700; });
    Outcome samples = compare(a, other_samples, dir / "samples.json");
    CHECK_EQ(samples.code, 2);
    CHECK(samples.error.find("different sample sizes") != std::string::npos);
    fs::path other_player = copy_result(a, dir / "other-player.json", [](Json& d) { d["player"] = "normal_dist"; });
    Outcome other_players = compare(a, other_player, dir / "players.json");
    CHECK_EQ(other_players.code, 2);
    CHECK(other_players.error.find("different players") != std::string::npos);
    fs::path bin = dir / "x.bin";
    CHECK_EQ(run_cli(capture_args(bin)).code, 0);
    Outcome mixed = compare(a, bin, dir / "mixed.json");
    CHECK_EQ(mixed.code, 2);
    CHECK(mixed.error.find("cannot compare a player output (.bin) with a `run player` result JSON") == 0);
    CHECK_EQ(compare(bin, a, dir / "mixed2.json").code, 2);
    write_file(dir / "other.json", "{\"schema\": \"something-else\"}");
    Outcome unknown = compare(a, dir / "other.json", dir / "unknown.json");
    CHECK_EQ(unknown.code, 2);
    CHECK(unknown.error.find("other.json is neither a player output") != std::string::npos);
    for (const char* label : {"no-runs", "bad-hash", "wrong-flag", "wrong-sha", "wrong-count", "bad-samples"}) {
        std::string name = label;
        fs::path damaged = copy_result(a, dir / (name + ".json"), [&](Json& d) {
            if (name == "no-runs") d["runs"] = Json::array();
            if (name == "bad-hash") d["runs"][0]["sha256"] = "xyz";
            if (name == "wrong-flag") d["reproducible"] = false;
            if (name == "wrong-sha") d["output_sha256"] = std::string(64, 'e');
            if (name == "wrong-count") d["distinct_output_sha256"] = 2;
            if (name == "bad-samples") d["samples_per_block"] = "many";
        });
        Outcome outcome = compare(a, damaged, dir / (name + "-report.json"));
        if (outcome.code != 2 || outcome.error.find("invalid player result") != 0) {
            ::testing::report_failure(__FILE__, __LINE__, name + ": code " + std::to_string(outcome.code) + ": " + outcome.error);
        }
        CHECK(!fs::exists(dir / (name + "-report.json")));
    }
}

int main() { return testing::run_all_tests(); }
