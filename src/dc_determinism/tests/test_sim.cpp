// The simulator commands' building blocks: worker line parsing, the repetition runner against a
// synthetic worker (tool_fake_worker), reduce_shots, `compare simulator` and the Python text
// helpers they use.
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/commands/simulator.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/platform.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/compare.hpp"
#include "sim/runner.hpp"
#include "tempdir.hpp"
#include "testing.hpp"
#include "zip/zip.hpp"

using namespace dc_determinism;

namespace {

std::string worker_exe() {
    return path_to_utf8(executable_path().parent_path() / (is_windows() ? "tool_fake_worker.exe" : "tool_fake_worker"));
}

std::vector<std::string> command(const std::string& mode) { return {worker_exe(), "--mode", mode}; }

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const DcDeterminismError& error) {
        return error.what();
    }
    return std::string();
}

const std::string kDigestA(64, 'a');
const std::string kDigestB(64, 'b');

}  // namespace

TEST(parse_line_accepts_a_result) {
    Outcome outcome = parse_line("5 stopped 120 " + kDigestA + " " + kDigestB + " 777\r\n", 5);
    CHECK_EQ(outcome.status, std::string("stopped"));
    CHECK_EQ(outcome.steps, 120);
    CHECK_EQ(outcome.trajectory_digest, kDigestA);
    CHECK_EQ(outcome.final_digest, kDigestB);
    CHECK_EQ(outcome.sim_ns, 777);
    CHECK_EQ(parse_line("0 truncated 1 " + kDigestA + " " + kDigestB + " 0", 0).status, std::string("truncated"));
}

TEST(parse_line_rejects_bad_lines) {
    CHECK(error_of([] { parse_line("ERR 3 boom", 3); }).find("worker reported: ERR 3 boom") == 0);
    CHECK(error_of([] { parse_line("garbage", 1); }).find("unexpected worker output for position 1: 'garbage'") == 0);
    // Wrong position, bad status, short digest, non-numeric steps.
    CHECK(!error_of([&] { parse_line("4 stopped 1 " + kDigestA + " " + kDigestB + " 1", 5); }).empty());
    CHECK(!error_of([&] { parse_line("5 done 1 " + kDigestA + " " + kDigestB + " 1", 5); }).empty());
    CHECK(!error_of([&] { parse_line("5 stopped 1 abc " + kDigestB + " 1", 5); }).empty());
    CHECK(!error_of([&] { parse_line("5 stopped x " + kDigestA + " " + kDigestB + " 1", 5); }).empty());
    CHECK(!error_of([&] { parse_line("5 stopped 1 " + std::string(64, 'A') + " " + kDigestB + " 1", 5); }).empty());
}

TEST(text_helpers) {
    std::int64_t value = 0;
    CHECK(py_parse_int("12", value) && value == 12);
    CHECK(py_parse_int("-1_0", value) && value == -10);
    CHECK(py_parse_int("+007", value) && value == 7);
    CHECK(!py_parse_int("", value));
    CHECK(!py_parse_int("1__0", value));
    CHECK(!py_parse_int("_1", value));
    CHECK(!py_parse_int("1_", value));
    CHECK(!py_parse_int("1.5", value));
    CHECK(!py_parse_int("9223372036854775808", value));
    CHECK(py_parse_int("-9223372036854775808", value));
    CHECK_EQ(py_strip(" \t a b \r\n"), std::string("a b"));
    CHECK_EQ(py_split("  a  b\tc\n").size(), static_cast<std::size_t>(3));
    CHECK_EQ(py_repr(Json("it's")), std::string("\"it's\""));
    CHECK_EQ(py_repr(Json("msvc")), std::string("'msvc'"));
    CHECK_EQ(py_repr(Json(nullptr)), std::string("None"));
    CHECK_EQ(format_g(2.0), std::string("2"));
    CHECK_EQ(format_fixed(1.005, 2), std::string("1.00"));
    CHECK(!py_truthy(Json::object()) && py_truthy(Json("x")) && !py_truthy(Json(0)) && !py_truthy(Json(0.0)));
}

TEST(short_reason_cuts_at_the_first_detail) {
    CHECK_EQ(cli::short_reason("msvc loader hash mismatch: C:\\x\\y.dll"), std::string("msvc loader hash mismatch"));
    CHECK_EQ(cli::short_reason("first line\nsecond: detail"), std::string("first line"));
    CHECK_EQ(cli::short_reason("ends with colon::"), std::string("ends with colon"));
    CHECK_EQ(cli::short_reason(""), std::string("DcDeterminismError"));
}

TEST(runner_collects_outcomes_in_position_order) {
    std::vector<std::size_t> reported;
    auto result = run_repetition(command("ok"), 25, 4, 0.0, [&](std::size_t done) { reported.push_back(done); });
    CHECK_EQ(result.second.size(), static_cast<std::size_t>(25));
    for (std::size_t i = 0; i < result.second.size(); ++i) {
        CHECK_EQ(result.second[i].steps, static_cast<std::int64_t>(100 + i));
        CHECK_EQ(result.second[i].trajectory_digest, sha256_hex("T" + std::to_string(i)));
    }
    CHECK(!reported.empty() && reported.back() == 25);
    CHECK(result.first >= 0.0);
}

TEST(runner_runs_selected_positions) {
    std::vector<std::int64_t> positions = {7, 3, 11};
    RunHooks hooks;
    hooks.positions = &positions;
    auto result = run_repetition(command("ok"), positions.size(), 2, 0.0, nullptr, hooks);
    CHECK_EQ(result.second[0].steps, 107);
    CHECK_EQ(result.second[1].steps, 103);
    CHECK_EQ(result.second[2].steps, 111);
}

TEST(runner_failure_modes_end_promptly) {
    auto start = std::chrono::steady_clock::now();
    CHECK(error_of([] { run_repetition(command("err"), 6, 3, 0.0, nullptr); }).find("simulated failure") !=
          std::string::npos);
    CHECK(error_of([] { run_repetition(command("eof"), 6, 3, 0.0, nullptr); }).find("ended unexpectedly") !=
          std::string::npos);
    CHECK(error_of([] { run_repetition(command("bad-line"), 6, 3, 0.0, nullptr); })
              .find("unexpected worker output") != std::string::npos);
    CHECK(error_of([] { run_repetition(command("startup-fail"), 6, 3, 0.0, nullptr); }).find("ERR -1") !=
          std::string::npos);
    CHECK_EQ(error_of([] { run_repetition(command("exit-3"), 6, 3, 0.0, nullptr); }), std::string("worker exited with code 3"));
    std::string timeout = error_of([] { run_repetition(command("hang"), 6, 3, 1.0, nullptr); });
    CHECK(timeout.find("repetition timed out after 1 s (0/6 shots done)") == 0);
    CHECK(error_of([] { run_repetition({"definitely-not-a-program-dc_determinism"}, 6, 3, 0.0, nullptr); })
              .find("cannot start worker") == 0);
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(seconds < 30.0);
}

TEST(reduce_shots_flags_differences) {
    Json shots = Json::array();
    for (int i = 0; i < 2; ++i) {
        Json shot = Json::object();
        shot["index"] = 10 + i;
        shot["shot_id"] = "s" + std::to_string(i);
        shot["fgz"] = i == 0 ? Json(nullptr) : Json("g.fgz");
        shots.push_back(shot);
    }
    Outcome a{"stopped", 5, kDigestA, kDigestA, 1};
    Outcome b{"stopped", 5, kDigestA, kDigestB, 1};
    Outcome c{"truncated", 5, kDigestB, kDigestA, 1};
    Json rows = reduce_shots(shots, {{a, a}, {a, b}, {a, c}});
    CHECK_EQ(rows[0]["shot_id"], Json("s0"));
    CHECK_EQ(rows[0]["reproducible_trajectory"], Json(true));
    CHECK_EQ(rows[0]["reproducible_final"], Json(true));
    CHECK_EQ(rows[0]["all_stopped"], Json(true));
    CHECK_EQ(rows[1]["reproducible_trajectory"], Json(false));
    CHECK_EQ(rows[1]["reproducible_final"], Json(false));
    CHECK_EQ(rows[1]["distinct_trajectory_digests"], Json(2));
    CHECK_EQ(rows[1]["distinct_final_digests"], Json(2));
    CHECK_EQ(rows[1]["all_stopped"], Json(false));
    CHECK_EQ(rows[1]["fgz"], Json("g.fgz"));
}

namespace {

Json result_file(const std::string& loader_sha, const std::vector<std::string>& digests) {
    Json doc = Json::object();
    doc["schema"] = kEvaluationSchema;
    Json contract = Json::object();
    contract["engine"] = "fcv1";
    contract["observation"] = "raw-stonecoordinate-v1";
    contract["max_steps"] = 60000;
    contract["record_digest"] = kDigestA;
    contract["corpus_input_sha256"] = kDigestB;
    contract["loader_sha256"] = loader_sha;
    doc["contract"] = contract;
    Json loader = Json::object();
    loader["origin"] = "prebuilt";
    Json prebuilt = Json::object();
    Json source = Json::object();
    source["tree_sha256_lf"] = kDigestA;
    prebuilt["source"] = source;
    prebuilt["dependencies"] = Json::object();
    loader["dc_prebuilt"] = prebuilt;
    doc["loader"] = loader;
    doc["worker"] = Json::object();
    doc["environment"] = Json::object();
    doc["repeats"] = 2;
    Json shots = Json::array();
    for (std::size_t i = 0; i < digests.size(); ++i) {
        Json row = Json::object();
        row["shot_id"] = "s" + std::to_string(i);
        row["status"] = "stopped";
        row["steps"] = 10;
        row["trajectory_digest"] = digests[i];
        row["final_digest"] = kDigestA;
        row["reproducible_trajectory"] = true;
        row["reproducible_final"] = true;
        shots.push_back(row);
    }
    doc["shots"] = shots;
    return doc;
}

}  // namespace

TEST(compare_results_counts_mismatches) {
    Json a = result_file(kDigestA, {kDigestA, kDigestA, kDigestB});
    Json b = result_file(kDigestB, {kDigestA, kDigestB, kDigestB});
    Json report = compare_results(a, b, kDigestA, kDigestB);
    CHECK_EQ(report["schema"], Json("dc-determinism-comparison-v2"));
    CHECK_EQ(report["inputs"][0]["sha256"], Json(kDigestA));
    CHECK_EQ(report["inputs"][1]["sha256"], Json(kDigestB));
    CHECK(!testing::has_path_key(report["inputs"]));
    CHECK_EQ(report["classification"], Json("cross-binary"));
    CHECK_EQ(report["trajectory"]["mismatches"], Json(1));
    CHECK_EQ(report["trajectory"]["mismatch_shots"][0], Json("s1"));
    CHECK_EQ(report["final"]["mismatches"], Json(0));
    CHECK_EQ(report["identical"], Json(false));
    Json same = compare_results(a, a, kDigestA, kDigestA);
    CHECK_EQ(same["identical"], Json(true));
    CHECK_EQ(same["classification"], Json("same-binary"));
    Json other = result_file(kDigestA, {kDigestA});
    CHECK(error_of([&] { compare_results(a, other, kDigestA, kDigestB); }).find("shot lists differ") !=
          std::string::npos);
    Json steps = a;
    steps["contract"]["max_steps"] = 7;
    CHECK(error_of([&] { compare_results(a, steps, kDigestA, kDigestB); })
              .find("contract max_steps differs (60000 vs 7)") != std::string::npos);
}

TEST(compare_simulator_json_mode_reports_the_input_digests_without_paths) {
    testing::TempDir dir;
    fs::path a = dir / "a.json";
    fs::path b = dir / "b.json";
    fs::path out = dir / "out.json";
    Json document = result_file(kDigestA, {kDigestA, kDigestB});
    document["loader"]["path"] = "/loader/path.so";  // a recorded path is not copied
    write_file_new(a, json_dumps(document));
    write_file_new(b, json_dumps(result_file(kDigestB, {kDigestA, kDigestA})));
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    cli::ParsedArgs args = parser->parse({"compare", "simulator", path_text(a), path_text(b), "--output", path_text(out)});
    CHECK_EQ(cli::dispatch(args), 1);
    Json report = read_json(out);
    CHECK_EQ(report["schema"], Json("dc-determinism-comparison-v2"));
    CHECK_EQ(report["inputs"][0]["sha256"], Json(sha256_hex(read_file(a))));
    CHECK_EQ(report["inputs"][1]["sha256"], Json(sha256_hex(read_file(b))));
    CHECK(!testing::has_path_key(report["inputs"]));
    std::vector<std::string> keys;
    for (auto it = report["inputs"][0].begin(); it != report["inputs"][0].end(); ++it) keys.push_back(it.key());
    CHECK((keys == std::vector<std::string>{"sha256", "loader", "worker", "environment", "repeats", "workers"}));
}

TEST(compare_simulator_json_mode_refuses_another_schema_name) {
    testing::TempDir dir;
    Json other = result_file(kDigestA, {kDigestA, kDigestB});
    other["schema"] = "dc-determinism-evaluation-v2";
    fs::path bad = dir / "bad.json";
    write_file_new(bad, json_dumps(other));
    CHECK(error_of([&] { load_result(path_text(bad)); }).find("schema is not dc-determinism-evaluation-v1") != std::string::npos);
}

TEST(compare_simulator_refuses_a_loader_that_is_not_prebuilt) {
    Json a = result_file(kDigestA, {kDigestA, kDigestB});
    Json b = a;
    b["loader"]["origin"] = "source-build";
    CHECK(error_of([&] { compare_results(a, b, kDigestA, kDigestB); })
              .find("incompatible results: the loader origin is 'source-build', but only \"prebuilt\" loaders are supported") == 0);
}

TEST(is_archive_detects_zip_files) {
    testing::TempDir dir;
    fs::path zip = dir.path() / "a.zip";
    {
        ZipWriter writer(zip);
        writer.add("x.txt", "hello");
        writer.finish();
    }
    CHECK(is_archive(zip));
    CHECK(!is_archive(dir.path() / "missing.zip"));
    fs::path text = dir.path() / "a.json";
    write_file_new(text, "{}");
    CHECK(!is_archive(text));
}

int main() { return testing::run_all_tests(); }
