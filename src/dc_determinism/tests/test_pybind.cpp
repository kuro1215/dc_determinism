// Unit and command tests of the pybind features (`compare pybind`, `compare pybind --check-recording`,
// `capture simulator --engine pybind-*`). No Python: the pybind worker is the synthetic tool_fake_pybind (same
// JSON protocol, `.f32` frame files) and the C API worker the synthetic tool_fake_worker. The worker driving
// (batches, timeouts, the shot-by-shot retry) is tested on every platform; the commands run end to end only where
// they are allowed (linux-amd64, with the prebuilt gcc loader named for its manifest check); elsewhere the tests
// check the refusal.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "capture/archive.hpp"
#include "capture/dctr.hpp"
#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/commands/pybind.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "corpus/capi_initial.hpp"
#include "corpus/catalogue.hpp"
#include "corpus/prepared.hpp"
#include "fake_board.hpp"
#include "json/writer.hpp"
#include "pybind/pybind.hpp"
#include "sim/metrics.hpp"
#include "sim/provenance.hpp"
#include "tempdir.hpp"
#include "testing.hpp"
#include "zip/zip.hpp"

using namespace dc_determinism;

namespace {

struct Outcome {
    int code = 0;
    std::string error;
};

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

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

fs::path tool(const char* name) {
    return executable_path().parent_path() / (is_windows() ? std::string(name) + ".exe" : std::string(name));
}

std::string join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (const std::string& id : ids) out += (out.empty() ? "" : ",") + id;
    return out;
}

// `--pybind-worker-command`: tool_fake_pybind with its options.
std::string pybind_hook(const std::vector<std::string>& ids, const std::vector<std::string>& options = {}) {
    Json command = Json::array();
    command.push_back(path_text(tool("tool_fake_pybind")));
    command.push_back("--shots");
    command.push_back(join_ids(ids));
    for (const std::string& option : options) command.push_back(option);
    return json_dumps(command);
}

// `--worker-command`: tool_fake_worker.
std::string capi_hook(const std::vector<std::string>& options = {}) {
    Json command = Json::array();
    command.push_back(path_text(tool("tool_fake_worker")));
    for (const std::string& option : options) command.push_back(option);
    return json_dumps(command);
}

std::vector<std::string> numbered_ids(int count) {
    std::vector<std::string> ids;
    for (int i = 0; i < count; ++i) ids.push_back("s" + std::to_string(i));
    return ids;
}

std::vector<std::pair<std::int64_t, Json>> numbered_tasks(const std::vector<std::string>& ids, bool with_width = false) {
    std::vector<std::pair<std::int64_t, Json>> tasks;
    for (std::size_t i = 0; i < ids.size(); ++i) {
        Json task = Json::object();
        task["shot_id"] = ids[i];
        task["angular_velocity"] = 1.5;
        if (with_width) task["removal_sheet_width"] = 4.5;
        tasks.emplace_back(static_cast<std::int64_t>(i), task);
    }
    return tasks;
}

std::vector<Json> read_calls(const fs::path& log_file) {
    std::vector<Json> calls;
    std::ifstream stream(log_file);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty()) calls.push_back(Json::parse(line));
    }
    return calls;
}

// A "binary" the fake worker never loads, with the SHA-256 the calls verify.
struct FakeBinary {
    testing::TempDir dir;
    PybindBinary binary;
    FakeBinary() {
        fs::path path = dir / "simulator-fake.so";
        write_file_new(path, "fake extension");
        binary = {path, sha256_hex("fake extension")};
    }
};

// The pybind final board of a fake board: [[8 points], [8 points]].
Json final_of_fake(const fakeboard::Board& board) {
    std::vector<std::array<double, 2>> points = fakeboard::to_final_points(board);
    Json out = Json::array();
    for (std::size_t team = 0; team < 2; ++team) {
        Json list = Json::array();
        for (std::size_t i = 0; i < 8; ++i) list.push_back(Json::array({points[team * 8 + i][0], points[team * 8 + i][1]}));
        out.push_back(list);
    }
    return out;
}

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const DcDeterminismError& error) {
        return error.what();
    }
    return std::string();
}

}  // namespace

// --- platform and options ------------------------------------------------------------------------

TEST(only_linux_amd64_may_run_the_pybind_features) {
    CHECK_EQ(pybind_platform_refusal("linux-amd64"), std::string());
    CHECK(contains(pybind_platform_refusal("windows-amd64"), "requires Linux"));
    CHECK(contains(pybind_platform_refusal("linux-arm64"), "x86-64"));
    CHECK(!pybind_platform_refusal("macos-arm64").empty());
    std::string error = error_of([] { require_pybind_platform("compare pybind"); });
    if (pybind_platform_refusal(platform_key()).empty()) {
        CHECK_EQ(error, std::string());
    } else {
        CHECK(error.compare(0, 14, "compare pybind") == 0);
    }
}

TEST(pybind_options_are_checked_before_anything_runs) {
    testing::TempDir dir;
    std::string out = path_text(dir / "report.json");
    std::vector<std::string> base = {"compare", "pybind", "--prepared", "p", "--logs-dir", "l", "--output", out};
    auto with = [&](std::vector<std::string> extra) {
        std::vector<std::string> words = base;
        words.insert(words.end(), extra.begin(), extra.end());
        return run_cli(words);
    };
    Outcome no_compiler = with({});
    CHECK_EQ(no_compiler.code, 2);
    CHECK(contains(no_compiler.error, "--compiler is required"));
    Outcome record = with({"--compiler", "gcc", "--record-py27", "x.so"});
    CHECK_EQ(record.code, 2);
    CHECK(contains(record.error, "--record-py27/--record-py36 need --trajectory-shots"));
    for (const char* option : {"--removal-sheet-width", "--frame-stride", "--limit", "--engines"}) {
        Outcome stray = with({"--compiler", "gcc", option, option == std::string("--engines") ? "py27" : "1"});
        CHECK_EQ(stray.code, 2);
        CHECK(contains(stray.error, std::string(option) + " applies to --check-recording only"));
    }
    for (std::vector<std::string> extra : {std::vector<std::string>{"--compiler", "gcc"},
                                           std::vector<std::string>{"--trajectory-shots", "t.txt"}}) {
        extra.insert(extra.begin(), "--check-recording");
        Outcome check = with(extra);
        CHECK_EQ(check.code, 2);
        CHECK(contains(check.error, "does not apply to --check-recording"));
    }
    CHECK(!fs::exists(dir / "report.json"));
    // an existing report is never overwritten
    write_file_new(dir / "existing.json", "{}");
    Outcome refused = run_cli({"compare", "pybind", "--prepared", "p", "--logs-dir", "l", "--compiler", "gcc",
                               "--output", path_text(dir / "existing.json")});
    CHECK_EQ(refused.code, 2);
    CHECK(contains(refused.error, "refusing to overwrite"));
}

TEST(the_pybind_commands_are_refused_where_they_cannot_run) {
    if (pybind_platform_refusal(platform_key()).empty()) return;
    testing::TempDir dir;
    Outcome compare = run_cli({"compare", "pybind", "--prepared", "p", "--logs-dir", "l", "--compiler", "gcc",
                               "--output", path_text(dir / "a.json")});
    CHECK_EQ(compare.code, 2);
    CHECK(contains(compare.error, "compare pybind requires"));
    Outcome check = run_cli({"compare", "pybind", "--check-recording", "--prepared", "p", "--logs-dir", "l", "--output",
                             path_text(dir / "b.json")});
    CHECK_EQ(check.code, 2);
    CHECK(contains(check.error, "--check-recording requires"));
    Outcome capture = run_cli({"capture", "simulator", "--prepared", "p", "--engine", "pybind-py27", "--logs-dir", "l",
                               "--all", "--output", path_text(dir / "c.zip")});
    CHECK_EQ(capture.code, 2);
    CHECK(contains(capture.error, "capture --engine pybind-py27 requires"));
    CHECK(!fs::exists(dir / "a.json") && !fs::exists(dir / "b.json") && !fs::exists(dir / "c.zip"));
}

TEST(capture_options_of_the_two_engine_families_are_kept_apart) {
    testing::TempDir dir;
    std::string out = path_text(dir / "c.zip");
    Outcome pybind_compiler = run_cli({"capture", "simulator", "--prepared", "p", "--engine", "pybind-py27",
                                       "--logs-dir", "l", "--compiler", "gcc", "--all", "--output", out});
    CHECK_EQ(pybind_compiler.code, 2);
    CHECK(contains(pybind_compiler.error, "--compiler applies to the C API engines only, not to pybind-py27"));
    Outcome pybind_expect = run_cli({"capture", "simulator", "--prepared", "p", "--engine", "pybind-py36", "--logs-dir",
                                     "l", "--expect", "r.json", "--all", "--output", out});
    CHECK(contains(pybind_expect.error, "--expect applies to the C API engines only"));
    Outcome no_logs = run_cli({"capture", "simulator", "--prepared", "p", "--engine", "pybind-py27", "--all", "--output", out});
    CHECK(contains(no_logs.error, "requires --logs-dir"));
    Outcome capi_logs = run_cli({"capture", "simulator", "--prepared", "p", "--engine", "fcv1", "--compiler", "gcc",
                                 "--logs-dir", "l", "--all", "--output", out});
    CHECK(contains(capi_logs.error, "--logs-dir applies to the pybind engines only"));
}

// --- pure helpers --------------------------------------------------------------------------------

TEST(tournament_keys) {
    std::string root = is_windows() ? "C:\\data\\logs" : "/data/logs";
    char sep = is_windows() ? '\\' : '/';
    auto below = [&](const std::string& relative) {
        std::string path = root;
        for (char c : relative) path += c == '/' ? sep : c;
        return path;
    };
    CHECK_EQ(tournament_of(below("/gat23-04a8-1.zip"), root), std::string("gat23"));
    CHECK_EQ(tournament_of(below("/si1-aa.zip"), root), std::string("si1"));
    CHECK_EQ(tournament_of(below("/sit1-aa.zip"), root), std::string("sit1"));
    CHECK_EQ(tournament_of(below("/GAT24-x.zip"), root), std::string("gat24"));
    CHECK_EQ(tournament_of(below("/sit1/game/shot_e0_s0.json"), root), std::string("sit1"));
    CHECK_EQ(tournament_of(below("/2024/gat23-x.zip"), root), std::string("gat23"));
    CHECK_EQ(tournament_of(below("/a/b/si1-y/g/shot.json"), root), std::string("si1"));
    CHECK_EQ(tournament_of(below("/weird.zip"), root), std::string("unknown"));
    CHECK_EQ(tournament_of(below("/g0/shot_e0_s0.json"), root), std::string("g0"));
    CHECK_EQ(tournament_of(is_windows() ? "C:\\elsewhere\\gat24-q.zip" : "/elsewhere/gat24-q.zip", root),
             std::string("gat24"));
    CHECK_EQ(tournament_of("", root), std::string("unknown"));
    CHECK_EQ(tournament_of(is_windows() ? "C:\\x\\y\\si1-a.zip" : "/x/y/si1-a.zip", ""), std::string("si1"));
}

TEST(pybind_final_boards_map_onto_the_c_api_slots) {
    Json final_board = Json::array();
    Json team0 = Json::array(), team1 = Json::array();
    for (int i = 0; i < 8; ++i) team0.push_back(Json::array({static_cast<double>(i + 1), 2.0}));
    for (int i = 0; i < 7; ++i) team1.push_back(Json::array({0.0, 0.0}));
    team1.push_back(Json::array({5.5, 6.5}));
    final_board.push_back(team0);
    final_board.push_back(team1);
    Board board = pybind_board(final_board);
    CHECK(board[0].present && board[0].x == 1.0 && board[0].y == 2.0);
    CHECK(board[7].present && board[7].x == 8.0);
    CHECK(!board[8].present);
    CHECK(board[15].present && board[15].x == 5.5 && board[15].y == 6.5);

    // absent: both coordinates within 1e-7; short teams are padded with absent stones
    Json odd = Json::array({Json::array({Json::array({1e-7, -1e-7}), Json::array({1.1e-7, 0.0}), Json::array({0.0, 2e-7})}),
                            Json::array({Json::array({3.0, 4.0})})});
    Board odd_board = pybind_board(odd);
    CHECK(!odd_board[0].present && odd_board[1].present && odd_board[2].present);
    CHECK(odd_board[8].present && !odd_board[9].present);
}

TEST(bad_pybind_final_boards_are_rejected) {
    auto points = [](int count) {
        Json list = Json::array();
        for (int i = 0; i < count; ++i) list.push_back(Json::array({1.0, 2.0}));
        return list;
    };
    for (const Json& bad : {Json(nullptr), Json::array({Json::array()}),
                            Json::array({Json::array(), Json::array(), Json::array()}),
                            Json::array({points(9), Json::array()}),
                            Json::array({Json::array({Json::array({1.0})}), Json::array()}),
                            Json::array({Json::array({Json::array({"a", 1.0})}), Json::array()}),
                            Json("xx")}) {
        CHECK_THROWS(pybind_board(bad));
    }
}

TEST(group_summaries_exclude_the_inconclusive_entries) {
    Board a, b;
    a[0] = {true, 1.0, 1.0};
    b[0] = {true, 1.0, 1.02};
    Json done = Json::object();
    done["status"] = "compared";
    Json boards = compare_boards(a, b);
    for (auto it = boards.begin(); it != boards.end(); ++it) done[it.key()] = it.value();
    Json outcome = Json::object();
    outcome["no1_equal"] = true;
    outcome["scores_equal"] = false;
    done["outcome"] = outcome;
    Json inconclusive = Json::object();
    inconclusive["status"] = "inconclusive";
    inconclusive["reason"] = "timeout";
    Json summary = pybind_group_summary({&done, &inconclusive});
    CHECK_EQ(summary["shots"].get<int>(), 2);
    CHECK_EQ(summary["conclusive"].get<int>(), 1);
    CHECK_EQ(summary["inconclusive"].get<int>(), 1);
    CHECK_EQ(summary["inconclusive_reasons"]["timeout"].get<int>(), 1);
    CHECK_EQ(summary["shots_d_gt_0"].get<int>(), 1);
    CHECK_EQ(summary["shots_d_gt_1cm"].get<int>(), 1);
    CHECK_EQ(summary["outcome"]["scores_changed"].get<int>(), 1);
    CHECK_EQ(summary["shot_max_distance_m"]["count"].get<int>(), 1);
    CHECK_EQ(summary["centimetre_m"].get<double>(), 0.01);
}

TEST(final_boards_are_compared_bit_for_bit) {
    Json board = Json::parse("[[[1.5,-2.25],[1.5,-2.25]],[[0.0,3.0],[0.0,3.0]]]");
    CHECK(bitwise_final_equal(board, Json::parse(board.dump())));
    Json flipped = Json::parse("[[[1.5,-2.25],[1.5,-2.25]],[[-0.0,3.0],[0.0,3.0]]]");
    CHECK(board == flipped);  // 0.0 == -0.0 numerically ...
    CHECK(!bitwise_final_equal(board, flipped));  // ... but not bit for bit
    Json moved = Json::parse("[[[1.5,-2.25],[1.5,-2.2500000000000004]],[[0.0,3.0],[0.0,3.0]]]");
    CHECK(!bitwise_final_equal(board, moved));
    CHECK(!bitwise_final_equal(board, Json::parse("[[[1.5,-2.25]]]")));
    CHECK(!bitwise_final_equal(board, Json(nullptr)));
    CHECK(!bitwise_final_equal(board, Json::parse("[[[1.5],[1.5]],[[0.0],[0.0]]]")));
    CHECK(!bitwise_final_equal(board, Json::parse("[[[1.5,\"x\"],[1.5,-2.25]],[[0.0,3.0],[0.0,3.0]]]")));
}

TEST(frame_files_are_read_with_a_size_check) {
    testing::TempDir dir;
    std::string bytes;
    for (int i = 0; i < 3 * 16 * 6; ++i) fakeboard::put_float(bytes, static_cast<double>(i));
    fs::path path = dir / "a.f32";
    write_file_new(path, bytes);
    std::vector<float> frames = read_frames(path, Json(3));
    CHECK_EQ(frames.size(), static_cast<std::size_t>(3 * 16 * 6));
    CHECK_EQ(frames[5], 5.0f);
    CHECK_EQ(frames.back(), static_cast<float>(3 * 16 * 6 - 1));
    for (int wrong : {2, 4}) CHECK(contains(error_of([&] { read_frames(path, Json(wrong)); }), "expected"));
    for (const Json& bad : {Json(0), Json(-1), Json(true), Json(3.0), Json(nullptr), Json("3")}) {
        CHECK(contains(error_of([&] { read_frames(path, bad); }), "invalid frame_count"));
    }
    write_file_new(dir / "short.f32", bytes.substr(0, bytes.size() - 1));
    CHECK(!error_of([&] { read_frames(dir / "short.f32", Json(3)); }).empty());
    CHECK(!error_of([&] { read_frames(dir / "missing.f32", Json(3)); }).empty());
}

TEST(frames_become_a_dctr_payload_with_the_present_stones_only) {
    std::vector<float> frames(4 * 16 * 6, 0.0f);
    auto set = [&](int frame, int slot, std::vector<float> state) {
        for (int field = 0; field < 6; ++field) frames[(static_cast<std::size_t>(frame) * 16 + slot) * 6 + field] = state[field];
    };
    for (int step = 0; step < 4; ++step) {
        set(step, 3, {1.0f + step, 2.0f, 0.5f, 0.1f, 0.2f, 0.3f});
        set(step, 9, {5.0f, 6.0f, 0.0f, 0.0f, 0.0f, 0.0f});
    }
    set(0, 9, {0, 0, 0, 0, 0, 0});  // a stone that is absent in the first frame only
    std::int64_t stored = 0;
    std::string payload = frames_to_payload(frames, 4, "a", 7, "pybind-py27", &stored);
    CHECK_EQ(stored, 4);
    CapturedTrajectory trajectory(payload);
    CHECK_EQ(trajectory.frames(), 4);
    CHECK_EQ(trajectory.stones().size(), static_cast<std::size_t>(2));
    CHECK_EQ(trajectory.stones()[0], 3);
    CHECK_EQ(trajectory.stones()[1], 9);
    CHECK_EQ(trajectory.header()["engine"].get<std::string>(), std::string("pybind-py27"));
    std::string raw = trajectory.to_raw();
    CHECK_EQ(raw.size(), static_cast<std::size_t>(8 + 4 * 2 * 6 * 4));
    CHECK_EQ(static_cast<unsigned char>(raw[0]), static_cast<unsigned char>((1 << 3)));
    CHECK_THROWS(frames_to_payload(frames, 5, "a", 7, "pybind-py27"));
}

TEST(the_legacy_pybind_input_follows_the_old_log) {
    LoadedShot shot;
    shot.shot_id = "game/end:1/shot:3";
    shot.log = Json::parse(
        "{\"end\":1,\"shot\":3,\"actual_move\":{\"velocity\":{\"x\":0.25,\"y\":2.5},\"rotation\":\"CW\"},"
        "\"simulator_storage\":{\"stones\":[{\"position\":{\"x\":1.0,\"y\":2.0}},null,"
        "{\"position\":{\"x\":-0.5,\"y\":3.0}}]}}");
    Json task = normalize_shot(shot);
    CHECK_EQ(task["shot_id"].get<std::string>(), std::string("game/end:1/shot:3"));
    CHECK_EQ(task["total_shot_number"].get<int>(), 4);
    CHECK_EQ(task["shot_per_team"].get<int>(), 1);  // shot 3: team 1, index 1
    CHECK_EQ(task["team_number"].get<int>(), 1);
    CHECK_EQ(task["applied_rule"].get<int>(), 0);
    CHECK_EQ(task["velocity_x"].get<double>(), 0.25);
    CHECK_EQ(task["velocity_y"].get<double>(), 2.5);
    CHECK_EQ(task["angular_velocity"].get<double>(), 3.141592653589793 / 2.0);  // cw: +pi/2 (reversed convention)
    const Json& positions = task["initial_positions"];
    CHECK_EQ(positions.size(), static_cast<std::size_t>(32));
    CHECK_EQ(positions[0].get<double>(), -1.0);  // end 1: the board is mirrored
    CHECK_EQ(positions[1].get<double>(), -2.0 + 21.0315);
    CHECK_EQ(positions[2].get<double>(), 0.0);
    CHECK_EQ(positions[4].get<double>(), 0.5);
    CHECK_EQ(positions[5].get<double>(), -3.0 + 21.0315);
    CHECK_EQ(positions[31].get<double>(), 0.0);
    CHECK_EQ(adapt_task(task, "py27")["angular_velocity"].get<double>(), task["angular_velocity"].get<double>());
    CHECK_EQ(adapt_task(task, "py36")["angular_velocity"].get<double>(), -task["angular_velocity"].get<double>());

    // the first shot of an end starts from an empty board; ccw gives -pi/2
    LoadedShot first;
    first.shot_id = "g/end:0/shot:0";
    first.log = Json::parse("{\"end\":0,\"shot\":0,\"actual_move\":{\"velocity\":{\"x\":0,\"y\":2},\"rotation\":\"ccw\"},"
                            "\"simulator_storage\":{\"stones\":[{\"position\":{\"x\":1.0,\"y\":2.0}}]}}");
    Json empty = normalize_shot(first);
    for (const Json& value : empty["initial_positions"]) CHECK_EQ(value.get<double>(), 0.0);
    CHECK_EQ(empty["angular_velocity"].get<double>(), -3.141592653589793 / 2.0);
    CHECK_EQ(empty["total_shot_number"].get<int>(), 1);

    LoadedShot broken;
    broken.shot_id = "x";
    broken.log = Json::parse("{\"end\":0,\"shot\":1,\"actual_move\":{\"velocity\":{\"x\":0}}}");
    CHECK(contains(error_of([&] { normalize_shot(broken); }), "rotation"));
}

// --- the worker driving (fake pybind worker, every platform) ---------------------------------------

TEST(official_calls_run_in_batches_of_fifty) {
    FakeBinary fake;
    testing::TempDir dir;
    std::vector<std::string> ids = numbered_ids(120);
    PybindWorker worker(pybind_hook(ids, {"--log", path_text(dir / "calls.log")}));
    std::map<std::int64_t, Json> results = run_official(worker, "py27", fake.binary, numbered_tasks(ids), 60.0, 1);
    CHECK_EQ(results.size(), static_cast<std::size_t>(120));
    std::vector<std::size_t> sizes;
    for (const Json& call : read_calls(dir / "calls.log")) {
        sizes.push_back(call["ids"].size());
        CHECK_EQ(call["kind"].get<std::string>(), std::string("official"));
        CHECK_EQ(call["engine"].get<std::string>(), std::string("fcv1-py27"));
        CHECK_EQ(call["frame_stride"].get<int>(), 10);
        CHECK(!call["frames_dir"].get<bool>());
    }
    std::sort(sizes.begin(), sizes.end());
    CHECK((sizes == std::vector<std::size_t>{20, 50, 50}));
    CHECK_EQ(results[5]["shot_id"].get<std::string>(), std::string("s5"));
    CHECK_EQ(results[5]["final"], final_of_fake(fakeboard::pybind_board("fcv1-py27", 5)));
    CHECK(worker.python_version() == Json("3.99.0-fake"));
    CHECK(worker.numpy_version() == Json("9.9.9-fake"));
    // threads give the same results
    std::map<std::int64_t, Json> threaded = run_official(worker, "py27", fake.binary, numbered_tasks(ids), 60.0, 3);
    CHECK(threaded == results);
}

TEST(a_failed_official_batch_is_retried_shot_by_shot) {
    FakeBinary fake;
    testing::TempDir dir;
    std::vector<std::string> ids = numbered_ids(10);
    // a timeout of the batch (and of the shot alone): only the guilty shot stays inconclusive
    PybindWorker hanging(pybind_hook(ids, {"--log", path_text(dir / "hang.log"), "--hang", "s7", "--only-kind", "official"}));
    std::map<std::int64_t, Json> results = run_official(hanging, "py27", fake.binary, numbered_tasks(ids), 1.0, 1);
    CHECK_EQ(results[7]["inconclusive"].get<std::string>(), std::string("timeout"));
    int inconclusive = 0;
    for (const auto& item : results) inconclusive += item.second.contains("inconclusive") ? 1 : 0;
    CHECK_EQ(inconclusive, 1);
    std::vector<std::size_t> sizes;
    for (const Json& call : read_calls(dir / "hang.log")) sizes.push_back(call["ids"].size());
    CHECK((sizes == std::vector<std::size_t>{10, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}));

    // a batch that fails (nonzero exit) while its shots succeed alone: nothing is inconclusive
    PybindWorker flaky(pybind_hook(ids, {"--log", path_text(dir / "flaky.log"), "--fail-when-batch", "s3"}));
    results = run_official(flaky, "py36", fake.binary, numbered_tasks(ids), 60.0, 1);
    for (const auto& item : results) CHECK(!item.second.contains("inconclusive"));
    CHECK_EQ(read_calls(dir / "flaky.log").size(), static_cast<std::size_t>(11));

    // a shot that fails alone: the reason carries the worker's exit and stderr
    PybindWorker failing(pybind_hook(ids, {"--fail", "s2"}));
    results = run_official(failing, "py27", fake.binary, numbered_tasks(ids), 60.0, 1);
    CHECK(contains(results[2]["inconclusive"].get<std::string>(), "worker error: pybind worker exited 3"));
    CHECK(contains(results[2]["inconclusive"].get<std::string>(), "simulated pybind failure"));
}

TEST(singleton_and_tail_batches_get_the_shot_by_shot_retry_too) {
    FakeBinary fake;
    for (int count : {1, 51}) {
        testing::TempDir dir;
        std::vector<std::string> ids = numbered_ids(count);
        // the first call fails whatever it holds: a singleton batch is retried, the first batch of 50 as well
        PybindWorker worker(pybind_hook(ids, {"--log", path_text(dir / "calls.log"), "--fail-once", path_text(dir / "once")}));
        std::map<std::int64_t, Json> results = run_official(worker, "py27", fake.binary, numbered_tasks(ids), 60.0, 1);
        CHECK_EQ(results.size(), static_cast<std::size_t>(count));
        for (const auto& item : results) CHECK(!item.second.contains("inconclusive"));
        std::size_t calls = read_calls(dir / "calls.log").size();
        // the failing first call of the first batch, then one call per shot of that batch, then the tail
        CHECK_EQ(calls, count == 1 ? static_cast<std::size_t>(2) : static_cast<std::size_t>(1 + 50 + 1));
    }
}

TEST(bad_official_answers_are_inconclusive_and_a_changed_binary_is_fatal) {
    FakeBinary fake;
    std::vector<std::string> ids = numbered_ids(3);
    for (const char* mode : {"--garbage", "--wrong-schema", "--drop-row"}) {
        PybindWorker worker(pybind_hook(ids, {mode}));
        std::map<std::int64_t, Json> results = run_official(worker, "py27", fake.binary, numbered_tasks(ids), 60.0, 1);
        for (const auto& item : results) {
            std::string reason = item.second.contains("inconclusive") ? item.second["inconclusive"].get<std::string>() : "";
            CHECK(std::string(mode) == "--garbage" ? contains(reason, "invalid worker response")
                                                   : contains(reason, "incomplete or out-of-order worker response"));
        }
    }
    PybindWorker worker(pybind_hook(ids));
    PybindBinary tampered = fake.binary;
    tampered.sha256 = std::string(64, '0');
    CHECK(contains(error_of([&] { run_official(worker, "py27", tampered, numbered_tasks(ids), 60.0, 1); }),
                   "binary SHA-256 mismatch"));
    PybindWorker missing(json_dumps(Json::array({"/nonexistent/pybind-worker"})));
    std::map<std::int64_t, Json> results = run_official(missing, "py27", fake.binary, numbered_tasks(ids), 60.0, 1);
    CHECK(contains(results[0]["inconclusive"].get<std::string>(), "cannot start the worker"));
    CHECK(contains(error_of([] { PybindWorker bad("[1]"); }), "non-empty JSON array of strings"));
    CHECK(contains(error_of([] { PybindWorker bad("not json"); }), "invalid JSON"));
}

TEST(record_batches_are_consumed_whole_and_retried_shot_by_shot) {
    FakeBinary fake;
    std::vector<std::string> ids = numbered_ids(30);
    auto run = [&](const std::vector<std::string>& options, std::vector<std::int64_t>& consumed, const fs::path& log_file,
                   std::string& error) {
        std::vector<std::string> all = options;
        all.insert(all.end(), {"--log", path_text(log_file)});
        PybindWorker worker(pybind_hook(ids, all));
        std::mutex mutex;
        auto consume = [&](std::int64_t position, const Json& row, std::vector<float>&& frames) {
            std::lock_guard<std::mutex> lock(mutex);
            consumed.push_back(position);
            CHECK_EQ(row["frame_count"].get<std::int64_t>(), 101 + position);
            CHECK_EQ(frames.size(), static_cast<std::size_t>((101 + position) * 96));
        };
        error = error_of([&] {
            record_trajectories(worker, "py27", fake.binary, numbered_tasks(ids, true), 60.0, 1, consume);
        });
        std::sort(consumed.begin(), consumed.end());
    };
    auto sizes_of = [](const fs::path& log_file) {
        std::vector<std::size_t> sizes;
        for (const Json& call : read_calls(log_file)) {
            sizes.push_back(call["ids"].size());
            CHECK_EQ(call["kind"].get<std::string>(), std::string("record"));
            CHECK_EQ(call["frame_stride"].get<int>(), 1);
            CHECK(call["frames_dir"].get<bool>());
            CHECK_EQ(call["widths"][0].get<double>(), 4.5);
        }
        return sizes;
    };

    testing::TempDir dir;
    std::vector<std::int64_t> consumed;
    std::string error;
    run({}, consumed, dir / "ok.log", error);
    CHECK_EQ(error, std::string());
    CHECK_EQ(consumed.size(), static_cast<std::size_t>(30));
    CHECK((sizes_of(dir / "ok.log") == std::vector<std::size_t>{25, 5}));

    // the batch fails, every shot succeeds alone
    consumed.clear();
    run({"--fail-when-batch", "s7"}, consumed, dir / "batch.log", error);
    CHECK_EQ(error, std::string());
    CHECK_EQ(consumed.size(), static_cast<std::size_t>(30));
    CHECK_EQ(sizes_of(dir / "batch.log").size(), static_cast<std::size_t>(1 + 25 + 1));

    // a shot that fails alone ends its batch: the shots before it are recorded, the later ones of the batch are not
    // tried; the other batch is untouched; the error names the shot
    consumed.clear();
    run({"--fail", "s7"}, consumed, dir / "fail.log", error);
    CHECK(contains(error, "fcv1-py27 recording of shot s7 failed: recording worker failed: pybind worker exited 3"));
    // no new batch is started after the failure (one worker: the second batch never runs)
    CHECK_EQ(consumed.size(), static_cast<std::size_t>(7));
    CHECK(std::count(consumed.begin(), consumed.end(), 8) == 0 && std::count(consumed.begin(), consumed.end(), 26) == 0);
    CHECK_EQ(read_calls(dir / "fail.log").size(), static_cast<std::size_t>(1 + 8));

    // unreadable frames (one byte short) fail the batch like any other failure
    consumed.clear();
    run({"--bad-frames", "s2"}, consumed, dir / "frames.log", error);
    CHECK(contains(error, "recording of shot s2 failed: cannot read the frames of s2"));
    CHECK(contains(error, "expected"));
    CHECK_EQ(consumed.size(), static_cast<std::size_t>(2));

    // a timeout
    consumed.clear();
    {
        PybindWorker worker(pybind_hook(ids, {"--hang", "s26"}));
        std::mutex mutex;
        std::string timeout_error = error_of([&] {
            record_trajectories(worker, "py36", fake.binary, numbered_tasks(ids, true), 1.0, 1,
                                [&](std::int64_t position, const Json&, std::vector<float>&&) {
                                    std::lock_guard<std::mutex> lock(mutex);
                                    consumed.push_back(position);
                                });
        });
        CHECK(contains(timeout_error, "fcv1-py36 recording of shot s26 failed: timeout"));
        CHECK_EQ(consumed.size(), static_cast<std::size_t>(25 + 1));  // the first batch and s25 alone
    }
}

TEST(check_calls_return_the_rows_or_an_error_per_shot) {
    FakeBinary fake;
    testing::TempDir dir;
    std::vector<std::string> ids = numbered_ids(30);
    PybindWorker worker(pybind_hook(ids, {"--log", path_text(dir / "calls.log"), "--fail", "s9", "--only-kind", "record"}));
    std::map<std::int64_t, Json> official =
        run_rows(worker, "py27", fake.binary, "official", numbered_tasks(ids, true), 1, 60.0, 2, "py27 official");
    std::map<std::int64_t, Json> record =
        run_rows(worker, "py27", fake.binary, "record", numbered_tasks(ids, true), 5, 60.0, 2, "py27 record-v2");
    CHECK_EQ(official.size(), static_cast<std::size_t>(30));
    for (const auto& item : official) CHECK(item.second.contains("final") && !item.second.contains("error"));
    CHECK_EQ(record.size(), static_cast<std::size_t>(30));
    int errors = 0;
    for (const auto& item : record) {
        if (item.second.contains("error")) {
            ++errors;
            CHECK_EQ(item.first, 9);
            CHECK(contains(item.second["error"].get<std::string>(), "pybind worker exited 3"));
        } else {
            CHECK(item.second.contains("last_frame") && item.second.contains("steps"));
        }
    }
    CHECK_EQ(errors, 1);
    std::set<std::int64_t> strides;
    for (const Json& call : read_calls(dir / "calls.log")) {
        if (call["kind"] == "record") strides.insert(call["frame_stride"].get<std::int64_t>());
        CHECK(!call["frames_dir"].get<bool>());  // the check asks for no frame files
    }
    CHECK((strides == std::set<std::int64_t>{5}));
}

// --- the commands end to end (linux-amd64: the prebuilt gcc loader is named for its manifest check) ----------

namespace {

bool commands_run() { return pybind_platform_refusal(platform_key()).empty(); }

// Four one-shot games in zip files (tournaments gat23, unknown, si1, sit1 in discovery order), prepared.
struct World {
    testing::TempDir temp;
    fs::path logs, prepared;
    Json document;
    std::vector<std::string> ids;
    int runs = 0;

    // `broken`: the log of the game `misc` has no rotation in its actual_move (`prepare` defaults it, the legacy pybind
    // input does not).
    explicit World(bool broken = false) {
        logs = resolve(temp.path()) / "logs";
        prepared = resolve(temp.path()) / "prepared";
        for (const char* name : {"gat23-a", "si1-b", "sit1-c", "misc"}) {
            ZipWriter zip(logs / (std::string(name) + ".zip"));
            zip.add("config.json", "{\"game\":{\"setting\":{\"sheet_width\":4.5}}}");
            zip.add("shot_e000s00.json",
                    std::string("{\"log\":{\"game_id\":\"") + name +
                        "\",\"end\":0,\"shot\":0,\"actual_move\":{\"type\":\"shot\",\"velocity\":{\"x\":0.25,\"y\":2.5}" +
                        (broken && std::string(name) == "misc" ? "" : ",\"rotation\":\"ccw\"") + "}}}");
            zip.finish();
        }
        Json identity = Json::object();
        identity["implementation"] = "cpp";
        document = prepare_corpus(logs, 0, prepared, identity);
        for (const Json& shot : document["shots"]) ids.push_back(shot["shot_id"].get<std::string>());
    }

    // A file with the ids of the given prepared positions.
    fs::path id_file(const std::vector<int>& positions) {
        std::string text;
        for (int position : positions) text += ids[static_cast<std::size_t>(position)] + "\n";
        fs::path path = temp / ("ids" + std::to_string(++runs) + ".txt");
        write_file_new(path, text);
        return path;
    }
};

struct Run {
    Outcome outcome;
    Json report;
    fs::path calls;  // the log of the fake pybind worker
    fs::path output;
};

Run finish(World& world, std::vector<std::string> words, const std::string& output_name, const std::string& extension,
           const std::vector<std::string>& pybind_options) {
    Run run;
    int number = ++world.runs;
    run.output = world.temp / (output_name + std::to_string(number) + extension);
    run.calls = world.temp / ("calls" + std::to_string(number) + ".log");
    std::vector<std::string> options = pybind_options;
    options.insert(options.end(), {"--log", path_text(run.calls)});
    words.insert(words.end(), {"--output", path_text(run.output), "--pybind-worker-command", pybind_hook(world.ids, options)});
    run.outcome = run_cli(words);
    if (fs::exists(run.output) && extension == ".json") run.report = read_json(run.output);
    return run;
}

Run compare(World& world, const std::vector<std::string>& pybind_options = {},
            const std::vector<std::string>& extra = {}, const std::vector<std::string>& capi_options = {}) {
    std::vector<std::string> words = {"compare", "pybind", "--prepared", path_text(world.prepared), "--logs-dir",
                                      path_text(world.logs), "--compiler", "gcc", "--workers", "2", "--worker-command",
                                      capi_hook(capi_options)};
    words.insert(words.end(), extra.begin(), extra.end());
    return finish(world, words, "report", ".json", pybind_options);
}

Run check(World& world, const std::vector<std::string>& pybind_options = {}, const std::vector<std::string>& extra = {}) {
    std::vector<std::string> words = {"compare", "pybind", "--check-recording", "--prepared", path_text(world.prepared),
                                      "--logs-dir", path_text(world.logs), "--workers", "2"};
    words.insert(words.end(), extra.begin(), extra.end());
    return finish(world, words, "check", ".json", pybind_options);
}

Run capture(World& world, const std::string& engine, const std::vector<std::string>& pybind_options = {},
            const std::vector<std::string>& extra = {}) {
    std::vector<std::string> words = {"capture", "simulator", "--prepared", path_text(world.prepared), "--engine", engine,
                                      "--logs-dir", path_text(world.logs), "--workers", "2"};
    words.insert(words.end(), extra.begin(), extra.end());
    return finish(world, words, "capture", ".zip", pybind_options);
}

std::int64_t shots_of(const Json& summary, const char* member) { return summary[member].get<std::int64_t>(); }

}  // namespace

TEST(compare_pybind_reports_every_engine_and_tournament) {
    if (!commands_run()) return;
    World world;
    Run run = compare(world);
    CHECK_EQ(run.outcome.code, 0);
    CHECK_EQ(run.outcome.error, std::string());
    const Json& report = run.report;
    CHECK_EQ(report["schema"].get<std::string>(), std::string("dc-determinism-pybind-deviation-v4"));
    const Json& contract = report["contract"];
    CHECK_EQ(contract["path_a_engine"].get<std::string>(), std::string("fcv1"));
    CHECK_EQ(contract["pybind_official"]["py27"].get<std::string>(), manifest_entry("py27", "official")["sha256"].get<std::string>());
    CHECK_EQ(contract["pybind_official"]["py36"].get<std::string>(), manifest_entry("py36", "official")["sha256"].get<std::string>());
    CHECK(contract["pybind_record"].empty());
    CHECK(!contract.contains("py36_rotation"));
    CHECK_EQ(contract["prepared_sha256"], world.document["prepared_sha256"]);
    CHECK_EQ(contract["record_digest"], world.document["record_digest"]);

    const Json& provenance = report["provenance"];
    CHECK(provenance.contains("pybind_worker") && provenance["pybind_worker"].contains("command"));
    CHECK_EQ(provenance["python_version"].get<std::string>(), std::string("3.99.0-fake"));
    CHECK_EQ(provenance["numpy_version"].get<std::string>(), std::string("9.9.9-fake"));
    CHECK_EQ(provenance["input_normalization"]["profile"].get<std::string>(), std::string("legacy"));
    CHECK(!provenance["input_normalization"].contains("py36_rotation"));
    CHECK(!provenance["input_normalization"].contains("py27_angular_velocity"));
    CHECK_EQ(report["known_implementation_differences"].size(), static_cast<std::size_t>(2));
    for (const Json& text : report["known_implementation_differences"]) {
        CHECK(!contains(text.get<std::string>(), "dc_determinism") && !contains(text.get<std::string>(), "normalize_shot"));
    }
    CHECK(contains(report["known_implementation_differences"][0].get<std::string>(), "8 slots per team"));
    CHECK(contains(report["known_implementation_differences"][1].get<std::string>(), "within 1e-7 of zero"));
    CHECK_EQ(report["timeout_s"].get<double>(), 600.0);
    CHECK_EQ(report["workers"]["requested"].get<int>(), 2);
    CHECK_EQ(report["workers"]["effective"].get<int>(), 2);

    // both official binaries ran on all shots, one batch each
    std::vector<Json> calls = read_calls(run.calls);
    CHECK_EQ(calls.size(), static_cast<std::size_t>(2));
    std::map<std::string, Json> by_engine;
    for (const Json& call : calls) {
        CHECK_EQ(call["kind"].get<std::string>(), std::string("official"));
        CHECK_EQ(call["ids"].size(), static_cast<std::size_t>(4));
        by_engine[call["engine"].get<std::string>()] = call;
    }
    CHECK(by_engine.count("fcv1-py27") == 1 && by_engine.count("fcv1-py36") == 1);
    // py36 gets the negated angular velocity, py27 the legacy value (ccw: -pi/2)
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK_EQ(by_engine["fcv1-py27"]["angular"][i].get<double>(), -3.141592653589793 / 2.0);
        CHECK_EQ(by_engine["fcv1-py36"]["angular"][i].get<double>(), 3.141592653589793 / 2.0);
    }

    std::vector<std::string> tournaments;
    for (const Json& row : report["shots"]) tournaments.push_back(row["tournament"].get<std::string>());
    CHECK((tournaments == std::vector<std::string>{"gat23", "unknown", "si1", "sit1"}));
    CHECK_EQ(report["shots"][2]["path_a"]["status"].get<std::string>(), std::string("stopped"));
    CHECK_EQ(report["shots"][2]["path_a"]["steps"].get<int>(), 102);

    const Json& engines = report["sections"]["engines"];
    const Json& py27 = engines["py27"]["all"];
    CHECK_EQ(shots_of(py27, "conclusive"), 4);
    CHECK_EQ(shots_of(py27, "shots_d_gt_0"), 0);
    CHECK_EQ(shots_of(py27, "shots_presence_mismatch"), 0);
    CHECK_EQ(py27["outcome"]["scores_changed"].get<int>(), 0);
    int present = 0;
    for (int position = 0; position < 4; ++position) {
        for (const fakeboard::Point& point : fakeboard::board(position, 100 + position)) present += point.present ? 1 : 0;
    }
    CHECK_EQ(py27["stone_classes"]["both_present"].get<int>(), present);
    const Json& py36 = engines["py36"]["all"];
    CHECK_EQ(shots_of(py36, "conclusive"), 4);
    CHECK_EQ(shots_of(py36, "shots_d_gt_0"), 2);
    CHECK_EQ(shots_of(py36, "shots_d_gt_1cm"), 2);
    CHECK_EQ(shots_of(py36, "shots_presence_mismatch"), 1);
    CHECK_EQ(py36["stone_classes"]["a_only"].get<int>(), 1);
    CHECK(std::fabs(py36["shot_max_distance_m"]["max"].get<double>() - 0.02) < 1e-6);
    const Json& by_tournament = engines["py36"]["by_tournament"];
    std::vector<std::string> names;
    for (auto it = by_tournament.begin(); it != by_tournament.end(); ++it) names.push_back(it.key());
    CHECK((names == std::vector<std::string>{"gat23", "si1", "sit1", "unknown"}));
    CHECK_EQ(shots_of(by_tournament["gat23"], "shots_d_gt_0"), 0);
    CHECK_EQ(shots_of(by_tournament["si1"], "shots_d_gt_0"), 0);  // position 2 only drops a stone
    CHECK_EQ(shots_of(by_tournament["sit1"], "shots_d_gt_0"), 1);  // positions 1 (unknown) and 3 shift one
    CHECK_EQ(shots_of(by_tournament["unknown"], "shots_d_gt_0"), 1);
    CHECK_EQ(shots_of(by_tournament["si1"], "shots_presence_mismatch"), 1);
    CHECK(report["inconclusive"].empty());
    CHECK(report["trajectories"].empty());
    CHECK(!report["shots"][1]["py36"].contains("distances"));  // the per-stone maps are not kept in the rows
    CHECK_EQ(report["shots"][1]["py36"]["stones_gt_1cm"].get<int>(), 1);
    CHECK_EQ(report["shots"][2]["py36"]["stones_gt_1cm"].get<int>(), 0);
}

TEST(compare_pybind_marks_timeouts_worker_errors_and_truncated_path_a_inconclusive) {
    if (!commands_run()) return;
    World world;
    // a timeout of one shot: the batch and the shot alone time out, nothing else is affected
    Run run = compare(world, {"--hang", world.ids[1], "--only-kind", "official"}, {"--timeout", "1"});
    CHECK_EQ(run.outcome.code, 0);
    std::set<std::pair<std::string, std::string>> reasons;
    for (const Json& item : run.report["inconclusive"]) {
        reasons.insert({item["shot_id"].get<std::string>() + "/" + item["engine"].get<std::string>(),
                        item["reason"].get<std::string>()});
    }
    CHECK((reasons == std::set<std::pair<std::string, std::string>>{{world.ids[1] + "/py27", "timeout"},
                                                                    {world.ids[1] + "/py36", "timeout"}}));
    const Json& py36 = run.report["sections"]["engines"]["py36"]["all"];
    CHECK_EQ(shots_of(py36, "shots"), 4);
    CHECK_EQ(shots_of(py36, "conclusive"), 3);
    CHECK_EQ(shots_of(py36, "inconclusive"), 1);
    CHECK_EQ(py36["inconclusive_reasons"]["timeout"].get<int>(), 1);
    CHECK_EQ(shots_of(py36, "shots_d_gt_0"), 1);  // shot 1 (odd, timed out) is excluded
    CHECK_EQ(run.report["shots"][1]["py36"]["status"].get<std::string>(), std::string("inconclusive"));
    CHECK_EQ(run.report["timeout_s"].get<double>(), 1.0);

    // a worker that exits nonzero on one shot, and Path A truncated on another
    World second;
    Run errors = compare(second, {"--fail", second.ids[3], "--only-kind", "official"}, {}, {"--truncated", "2"});
    CHECK_EQ(errors.outcome.code, 0);
    std::map<std::string, std::string> why;
    for (const Json& item : errors.report["inconclusive"]) {
        why[item["shot_id"].get<std::string>() + "/" + item["engine"].get<std::string>()] = item["reason"].get<std::string>();
    }
    CHECK(contains(why[second.ids[3] + "/py27"], "worker error"));
    CHECK_EQ(why[second.ids[2] + "/py36"], std::string("path A truncated"));
    CHECK_EQ(shots_of(errors.report["sections"]["engines"]["py27"]["all"], "conclusive"), 2);
    CHECK_EQ(errors.report["shots"][2]["path_a"]["status"].get<std::string>(), std::string("truncated"));
}

TEST(compare_pybind_refuses_a_changed_binary_and_bad_options) {
    if (!commands_run()) return;
    World world;
    testing::TempDir other;
    write_file_new(other / "other.so", "not the extension");
    Run official = compare(world, {}, {"--official-py27", path_text(other / "other.so")});
    CHECK_EQ(official.outcome.code, 2);
    CHECK(contains(official.outcome.error, "is not the official fcv1-py27 binary"));
    CHECK(!fs::exists(official.output));
    Run record = compare(world, {}, {"--trajectory-shots", path_text(world.id_file({1})), "--record-py36",
                                     path_text(other / "other.so")});
    CHECK_EQ(record.outcome.code, 2);
    CHECK(contains(record.outcome.error, "is not the record fcv1-py36 binary"));
    Run missing = compare(world, {}, {"--official-py36", path_text(other / "absent.so")});
    CHECK(contains(missing.outcome.error, "official binary not found"));
    Run unknown = compare(world, {}, {"--trajectory-shots", path_text(other / "no-such-list.txt")});
    CHECK(contains(unknown.outcome.error, "cannot read shot list"));
    write_file_new(other / "ids.txt", "not/a/known/shot\n");
    Run not_prepared = compare(world, {}, {"--trajectory-shots", path_text(other / "ids.txt")});
    CHECK(contains(not_prepared.outcome.error, "lists 1 shots that are not in the prepared corpus"));
    Outcome bad_hook = run_cli({"compare", "pybind", "--prepared", path_text(world.prepared), "--logs-dir", path_text(world.logs),
                            "--compiler", "gcc", "--output", path_text(world.temp / "hook.json"),
                            "--pybind-worker-command", "{}"});
    CHECK(contains(bad_hook.error, "--pybind-worker-command must be a non-empty JSON array of strings"));
    // an existing report is not overwritten
    Run first = compare(world);
    CHECK_EQ(first.outcome.code, 0);
    std::string before = read_file(first.output);
    Outcome again = run_cli({"compare", "pybind", "--prepared", path_text(world.prepared), "--logs-dir", path_text(world.logs),
                             "--compiler", "gcc", "--output", path_text(first.output)});
    CHECK_EQ(again.code, 2);
    CHECK(contains(again.error, "refusing to overwrite"));
    CHECK_EQ(read_file(first.output), before);
}

TEST(compare_pybind_replays_the_listed_shots_with_the_recording_binaries) {
    if (!commands_run()) return;
    World world;
    Run run = compare(world, {"--shift", "40"}, {"--trajectory-shots", path_text(world.id_file({1}))});
    CHECK_EQ(run.outcome.code, 0);
    CHECK_EQ(run.outcome.error, std::string());
    const Json& report = run.report;
    std::vector<Json> record_calls;
    for (const Json& call : read_calls(run.calls)) {
        if (call["kind"] == "record") record_calls.push_back(call);
    }
    CHECK_EQ(record_calls.size(), static_cast<std::size_t>(2));  // one shot per call, one call per engine
    std::set<std::string> engines;
    for (const Json& call : record_calls) {
        engines.insert(call["engine"].get<std::string>());
        CHECK_EQ(call["ids"].size(), static_cast<std::size_t>(1));
        CHECK_EQ(call["ids"][0].get<std::string>(), world.ids[1]);
        CHECK_EQ(call["frame_stride"].get<int>(), 1);
        CHECK_EQ(call["widths"][0].get<double>(), 4.5);  // the sheet width of the prepared shot
        CHECK(call["frames_dir"].get<bool>());
    }
    CHECK((engines == std::set<std::string>{"fcv1-py27", "fcv1-py36"}));
    // py36 gets the reversed angular velocity
    std::map<std::string, double> angular;
    for (const Json& call : record_calls) angular[call["engine"].get<std::string>()] = call["angular"][0].get<double>();
    CHECK_EQ(angular["fcv1-py36"], -angular["fcv1-py27"]);

    CHECK_EQ(report["trajectories"].size(), static_cast<std::size_t>(2));
    std::map<std::string, Json> rows;
    for (const Json& row : report["trajectories"]) rows[row["engine"].get<std::string>()] = row;
    for (const char* engine : {"py27", "py36"}) {
        const Json& row = rows[engine];
        CHECK_EQ(row["status"].get<std::string>(), std::string("compared"));
        CHECK_EQ(row["shot_id"].get<std::string>(), world.ids[1]);
        CHECK_EQ(row["steps_a"].get<int>(), 101);
        CHECK_EQ(row["steps_b"].get<int>(), 101);
        CHECK_EQ(row["step_count_difference"].get<int>(), 0);
        CHECK_EQ(row["removed_steps"].get<int>(), 0);
        CHECK_EQ(row["frames_a"].get<int>(), row["frames_b"].get<int>());
        // all six fields are compared: only x of stone 1 differs, from frame 40 on
        CHECK_EQ(row["first_diff"]["step"].get<int>(), 40);
        CHECK_EQ(row["first_diff"]["fields"][0].get<std::string>(), std::string("x"));
        CHECK(std::fabs(row["max_diff"]["position"]["value"].get<double>() - 0.002) < 1e-5);
        CHECK_EQ(row["max_diff"]["velocity"]["value"].get<double>(), 0.0);
        CHECK(row.contains("outcome"));
        CHECK(row["recording_official_final_equal"] == true);  // bit for bit
    }
    CHECK(contains(report["trajectory_note"].get<std::string>(), "physical frames"));
    CHECK_EQ(report["contract"]["pybind_record"]["py27"].get<std::string>(),
             manifest_entry("py27", "record")["sha256"].get<std::string>());
}

TEST(compare_pybind_reports_what_the_recording_cannot_say) {
    if (!commands_run()) return;
    World world;
    // a recording final that differs from the official one, even by a unit in the last place of one coordinate
    Run differs = compare(world, {"--final-delta", "1.0"}, {"--trajectory-shots", path_text(world.id_file({1}))});
    CHECK_EQ(differs.outcome.code, 0);
    for (const Json& row : differs.report["trajectories"]) CHECK(row["recording_official_final_equal"] == false);

    // no official final for the shot: the bitwise check says null
    Run no_final = compare(world, {"--hang", world.ids[1], "--only-kind", "official"},
                           {"--trajectory-shots", path_text(world.id_file({1})), "--timeout", "1"});
    CHECK_EQ(no_final.outcome.code, 0);
    CHECK_EQ(no_final.report["trajectories"].size(), static_cast<std::size_t>(2));
    for (const Json& row : no_final.report["trajectories"]) {
        CHECK_EQ(row["status"].get<std::string>(), std::string("compared"));
        CHECK(row["recording_official_final_equal"].is_null());
    }

    // a recording failure: only the not yet recorded entries become inconclusive
    Run failing = compare(world, {"--fail", world.ids[1], "--only-kind", "record"},
                          {"--trajectory-shots", path_text(world.id_file({0, 1})), "--timeout", "30"});
    CHECK_EQ(failing.outcome.code, 0);
    CHECK_EQ(failing.report["trajectories"].size(), static_cast<std::size_t>(4));
    std::map<std::string, std::string> status;
    for (const Json& row : failing.report["trajectories"]) {
        status[row["shot_id"].get<std::string>() + "/" + row["engine"].get<std::string>()] = row["status"].get<std::string>();
        if (row["status"] == "inconclusive") CHECK(contains(row["reason"].get<std::string>(), "recording of shot"));
    }
    CHECK_EQ(status[world.ids[0] + "/py27"], std::string("compared"));  // recorded before the failing shot
    CHECK_EQ(status[world.ids[1] + "/py27"], std::string("inconclusive"));
    CHECK_EQ(status[world.ids[1] + "/py36"], std::string("inconclusive"));

    // Path A truncated: no capture to compare
    World truncated;
    Run no_capture = compare(truncated, {}, {"--trajectory-shots", path_text(truncated.id_file({2}))}, {"--truncated", "2"});
    CHECK_EQ(no_capture.outcome.code, 0);
    for (const Json& row : no_capture.report["trajectories"]) {
        CHECK_EQ(row["status"].get<std::string>(), std::string("inconclusive"));
        CHECK_EQ(row["reason"].get<std::string>(), std::string("no path A capture"));
    }
}

TEST(capture_pybind_writes_the_recording_frames_as_dctr) {
    if (!commands_run()) return;
    World world;
    for (const char* engine : {"pybind-py27", "pybind-py36"}) {
        Run run = capture(world, engine, {}, {"--shots", path_text(world.id_file({1, 3}))});
        CHECK_EQ(run.outcome.code, 0);
        CHECK_EQ(run.outcome.error, std::string());
        CaptureArchive archive(path_text(run.output));
        std::unique_ptr<ZipReader> reader = archive.open_reader();
        const Json& index = archive.index();
        CHECK_EQ(index["contract"]["engine"].get<std::string>(), std::string(engine));
        CHECK_EQ(archive.shot_ids().size(), static_cast<std::size_t>(2));
        CHECK_EQ(index["selection"]["count"].get<int>(), 2);
        CHECK_EQ(index["provenance"]["engine_binary"]["kind"].get<std::string>(), std::string("record-v2"));
        CHECK(index["provenance"]["pybind_worker"].contains("command"));
        CHECK_EQ(index["provenance"]["python_version"].get<std::string>(), std::string("3.99.0-fake"));
        CHECK_EQ(index["provenance"]["numpy_version"].get<std::string>(), std::string("9.9.9-fake"));
        CHECK(!index["provenance"]["input_normalization"].contains("py36_rotation"));
        CHECK(!index["provenance"]["input_normalization"].contains("py27_angular_velocity"));
        for (int position : {1, 3}) {
            const std::string& id = world.ids[static_cast<std::size_t>(position)];
            const Json& row = archive.row(id);
            CHECK_EQ(row["status"].get<std::string>(), std::string("stopped"));
            CHECK_EQ(row["steps"].get<int>(), 100 + position);
            CHECK(row["trajectory_digest"].is_null() && row["final_digest"].is_null());
            CHECK_EQ(row["frames"].get<int>(), 101 + position);
            CapturedTrajectory trajectory = archive.trajectory(id, *reader);
            CHECK_EQ(trajectory.frames(), 101 + position);
            CHECK_EQ(trajectory.header()["engine"].get<std::string>(), std::string(engine));
            std::vector<int> expected;
            for (int sid = 0; sid < 16; ++sid) {
                if (fakeboard::stone(position, 0, sid).present) expected.push_back(sid);
            }
            CHECK((trajectory.stones() == expected));
        }
        std::vector<Json> calls = read_calls(run.calls);
        CHECK_EQ(calls.size(), static_cast<std::size_t>(1));  // both shots in one batch
        CHECK_EQ(calls[0]["ids"].size(), static_cast<std::size_t>(2));
        CHECK_EQ(calls[0]["widths"][0].get<double>(), 4.5);
        CHECK_EQ(calls[0]["angular"][0].get<double>(),
                 std::string(engine) == "pybind-py36" ? 3.141592653589793 / 2.0 : -3.141592653589793 / 2.0);
    }
    // a recording failure ends the capture with an error and leaves no archive
    Run failing = capture(world, "pybind-py27", {"--fail", world.ids[3], "--only-kind", "record"}, {"--all"});
    CHECK_EQ(failing.outcome.code, 2);
    CHECK(contains(failing.outcome.error, "fcv1-py27 recording of shot " + world.ids[3] + " failed"));
    CHECK(!fs::exists(failing.output));
}

TEST(check_recording_compares_every_shot_with_the_official_final) {
    if (!commands_run()) return;
    World world;
    Run run = check(world);
    CHECK_EQ(run.outcome.code, 1);  // fcv1-py36 differs on the shots 1 and 3
    CHECK_EQ(run.outcome.error, std::string());
    const Json& report = run.report;
    CHECK_EQ(report["schema"].get<std::string>(), std::string("dc-recording-v2-check-v3"));
    CHECK_EQ(report["contract"]["shots"].get<int>(), 4);
    CHECK(!report["contract"].contains("py36_rotation"));
    CHECK_EQ(report["contract"]["frame_stride"].get<int>(), 1);
    CHECK_EQ(report["contract"]["removal_sheet_width"].get<std::string>(), std::string("none"));
    CHECK_EQ(report["contract"]["binaries"]["py27"]["record_v2_sha256"].get<std::string>(),
             manifest_entry("py27", "record")["sha256"].get<std::string>());
    CHECK(report["provenance"].contains("pybind_worker"));
    CHECK_EQ(report["provenance"]["python_version"].get<std::string>(), std::string("3.99.0-fake"));
    CHECK(report["input_errors"].empty());
    CHECK_EQ(report["errors"].get<int>(), 0);
    CHECK(report["all_finals_equal"] == false);

    const Json& py27 = report["engines"]["py27"];
    CHECK_EQ(py27["engine"].get<std::string>(), std::string("fcv1-py27"));
    CHECK_EQ(py27["shots"].get<int>(), 4);
    CHECK_EQ(py27["compared"].get<int>(), 4);
    CHECK_EQ(py27["equal"].get<int>(), 4);
    CHECK_EQ(py27["differing"].get<int>(), 0);
    CHECK_EQ(py27["final_return_equal"].get<int>(), 4);
    CHECK_EQ(py27["last_frame_equal"].get<int>(), 4);
    CHECK_EQ(py27["last_frame_strict_equal"].get<int>(), 4);
    CHECK_EQ(py27["max_distance_m"].get<double>(), 0.0);
    CHECK_EQ(py27["frame_count"]["count"].get<int>(), 4);
    CHECK_EQ(py27["frame_count"]["min"].get<int>(), 101);
    CHECK_EQ(py27["frame_count"]["max"].get<int>(), 104);
    CHECK_EQ(py27["frame_count"]["mean"].get<double>(), 102.5);
    CHECK_EQ(py27["end_velocity"]["shots_all_zero"].get<int>(), 4);  // the synthetic shots end at rest

    const Json& py36 = report["engines"]["py36"];
    CHECK_EQ(py36["equal"].get<int>(), 2);
    CHECK_EQ(py36["differing"].get<int>(), 2);
    CHECK_EQ(py36["final_return_equal"].get<int>(), 4);  // the recording binary returns the official final
    CHECK_EQ(py36["last_frame_equal"].get<int>(), 2);
    CHECK_EQ(py36["shots_with_official_removed_stones"].get<int>(), 1);  // shot 2: the official final drops a stone
    CHECK_EQ(py36["official_removed_stones"].get<int>(), 1);
    CHECK(std::fabs(py36["max_distance_m"].get<double>() - 0.02) < 1e-6);
    CHECK_EQ(py36["differing_shots"].size(), static_cast<std::size_t>(2));
    CHECK_EQ(py36["differing_shots"][0]["shot_id"].get<std::string>(), world.ids[1]);
    CHECK_EQ(py36["differing_shots"][0]["position_differs_slots"].size(), static_cast<std::size_t>(1));
    CHECK(py36["differing_shots"][0]["missing_in_record_slots"].empty());
    CHECK_EQ(py36["differing_shots"][1]["shot_id"].get<std::string>(), world.ids[3]);
    CHECK(py36["error_shots"].empty());

    // both kinds got the shots with a removal width of 0 and stride 1 (the official calls always 1)
    for (const Json& call : read_calls(run.calls)) {
        CHECK_EQ(call["frame_stride"].get<int>(), 1);
        for (const Json& width : call["widths"]) CHECK_EQ(width.get<double>(), 0.0);
        CHECK(!call["frames_dir"].get<bool>());
        CHECK(call["ids"].size() <= 25);
    }
}

TEST(check_recording_options) {
    if (!commands_run()) return;
    World world;
    // widths: shot = the sheet width of each prepared shot; metres; a frame stride, a limit and one engine
    Run shot = check(world, {}, {"--removal-sheet-width", "shot"});
    CHECK_EQ(shot.report["contract"]["removal_sheet_width"].get<std::string>(), std::string("shot"));
    for (const Json& call : read_calls(shot.calls)) {
        for (const Json& width : call["widths"]) CHECK_EQ(width.get<double>(), 4.5);
    }
    Run metres = check(world, {}, {"--removal-sheet-width", "0.9"});
    CHECK_EQ(metres.report["contract"]["removal_sheet_width"].get<std::string>(), std::string("0.9"));
    for (const Json& call : read_calls(metres.calls)) {
        for (const Json& width : call["widths"]) CHECK_EQ(width.get<double>(), 0.9);
    }
    Run narrow = check(world, {}, {"--frame-stride", "5", "--limit", "2", "--engines", "py36"});
    CHECK_EQ(narrow.outcome.code, 1);  // shots 0 and 1: fcv1-py36 differs on shot 1
    CHECK_EQ(narrow.report["contract"]["frame_stride"].get<int>(), 5);
    CHECK_EQ(narrow.report["contract"]["shots"].get<int>(), 2);
    CHECK_EQ(narrow.report["engines"].size(), static_cast<std::size_t>(1));
    CHECK(narrow.report["engines"].contains("py36"));
    for (const Json& call : read_calls(narrow.calls)) {
        CHECK_EQ(call["frame_stride"].get<int>(), call["kind"] == "record" ? 5 : 1);
        CHECK_EQ(call["ids"].size(), static_cast<std::size_t>(2));
    }
    Run bad_engine = check(world, {}, {"--engines", "py99"});
    CHECK_EQ(bad_engine.outcome.code, 2);
    CHECK(contains(bad_engine.outcome.error, "--engines takes py27 and/or py36"));
    Run bad_width = check(world, {}, {"--removal-sheet-width", "wide"});
    CHECK_EQ(bad_width.outcome.code, 2);
    CHECK(contains(bad_width.outcome.error, "--removal-sheet-width takes none, shot or a number of metres"));
    Run bad_stride = check(world, {}, {"--frame-stride", "0"});
    CHECK_EQ(bad_stride.outcome.code, 2);
    CHECK(!fs::exists(bad_stride.output));
}

TEST(check_recording_reports_a_shot_that_cannot_be_evaluated) {
    if (!commands_run()) return;
    World world;
    Run run = check(world, {"--fail", world.ids[1], "--only-kind", "record"}, {"--engines", "py27"});
    CHECK_EQ(run.outcome.code, 2);
    CHECK_EQ(run.report["errors"].get<int>(), 1);
    CHECK(run.report["all_finals_equal"] == false);
    const Json& engine = run.report["engines"]["py27"];
    CHECK_EQ(engine["shots"].get<int>(), 4);
    CHECK_EQ(engine["compared"].get<int>(), 3);
    CHECK_EQ(engine["errors"].get<int>(), 1);
    CHECK_EQ(engine["error_shots"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(engine["error_shots"][0]["shot_id"].get<std::string>(), world.ids[1]);
    CHECK(engine["error_shots"][0]["error"]["official"].is_null());
    CHECK(contains(engine["error_shots"][0]["error"]["record_v2"].get<std::string>(), "pybind worker exited 3"));
    // the failing shot alone was retried after its batch failed: 1 batch of 4, then 4 single calls
    std::size_t record_calls = 0;
    for (const Json& call : read_calls(run.calls)) record_calls += call["kind"] == "record" ? 1 : 0;
    CHECK_EQ(record_calls, static_cast<std::size_t>(1 + 4));
}

TEST(a_request_that_cannot_be_serialised_fails_only_its_shots) {
    FakeBinary fake;
    std::vector<std::string> ids = numbered_ids(4);
    PybindWorker worker(pybind_hook(ids));
    std::vector<std::pair<std::int64_t, Json>> tasks = numbered_tasks(ids, true);
    tasks[2].second["velocity_x"] = std::nan("");  // json.dumps(allow_nan=False) fails for this shot only
    std::map<std::int64_t, Json> results = run_official(worker, "py27", fake.binary, tasks, 60.0, 1);
    CHECK_EQ(results.size(), static_cast<std::size_t>(4));
    for (std::int64_t position : {0, 1, 3}) CHECK(results[position].contains("final"));
    CHECK(contains(results[2]["inconclusive"].get<std::string>(), "invalid worker response: "));
    std::map<std::int64_t, Json> rows = run_rows(worker, "py27", fake.binary, "record", tasks, 1, 60.0, 1, "py27 record-v2");
    CHECK(rows[0].contains("last_frame") && rows[3].contains("last_frame"));
    CHECK(contains(rows[2]["error"].get<std::string>(), "invalid recording worker output: "));
    std::string error = error_of([&] {
        record_trajectories(worker, "py27", fake.binary, tasks, 60.0, 1, [](std::int64_t, const Json&, std::vector<float>&&) {});
    });
    CHECK(contains(error, "fcv1-py27 recording of shot s2 failed: invalid recording worker output: "));
}

TEST(a_log_without_a_pybind_input_is_an_input_error_of_that_shot) {
    if (!commands_run()) return;
    World world(true);  // position 1 (`misc`) has no rotation
    const std::string expected = "ValueError: invalid shot log: missing 'rotation'";

    Run run = compare(world);
    CHECK_EQ(run.outcome.code, 0);
    CHECK_EQ(run.report["shots"][1]["input_error"].get<std::string>(), expected);
    CHECK(!run.report["shots"][0].contains("input_error"));
    for (const char* engine : {"py27", "py36"}) {
        CHECK_EQ(run.report["shots"][1][engine]["status"].get<std::string>(), std::string("inconclusive"));
        CHECK_EQ(run.report["shots"][1][engine]["reason"].get<std::string>(), std::string("input error"));
        const Json& summary = run.report["sections"]["engines"][engine]["all"];
        CHECK_EQ(shots_of(summary, "conclusive"), 3);
        CHECK_EQ(summary["inconclusive_reasons"]["input error"].get<int>(), 1);
    }
    CHECK_EQ(run.report["inconclusive"].size(), static_cast<std::size_t>(2));
    for (const Json& call : read_calls(run.calls)) {
        CHECK_EQ(call["ids"].size(), static_cast<std::size_t>(3));  // the shot without input is not sent
    }

    Run checked = check(world);
    CHECK_EQ(checked.outcome.code, 2);
    CHECK_EQ(checked.report["input_errors"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(checked.report["input_errors"][0]["shot_id"].get<std::string>(), world.ids[1]);
    CHECK_EQ(checked.report["input_errors"][0]["error"].get<std::string>(), expected);
    CHECK_EQ(checked.report["engines"]["py27"]["shots"].get<int>(), 3);
    CHECK_EQ(checked.report["errors"].get<int>(), 0);

    Run captured = capture(world, "pybind-py27", {}, {"--shots", path_text(world.id_file({0, 1}))});
    CHECK_EQ(captured.outcome.code, 2);
    CHECK_EQ(captured.outcome.error, "shot " + world.ids[1] + ": cannot build the pybind input: " + expected);
    CHECK(!fs::exists(captured.output));
}

TEST(the_official_final_is_kept_for_the_bitwise_check_even_when_it_is_no_board) {
    if (!commands_run()) return;
    World world;
    Run run = compare(world, {"--bad-final", "--only-kind", "official"}, {"--trajectory-shots", path_text(world.id_file({1}))});
    CHECK_EQ(run.outcome.code, 0);
    CHECK_EQ(run.report["shots"][1]["py27"]["status"].get<std::string>(), std::string("inconclusive"));
    CHECK(contains(run.report["shots"][1]["py27"]["reason"].get<std::string>(), "invalid final board: "));
    CHECK_EQ(run.report["trajectories"].size(), static_cast<std::size_t>(2));
    for (const Json& row : run.report["trajectories"]) {
        CHECK_EQ(row["status"].get<std::string>(), std::string("compared"));
        CHECK(row["recording_official_final_equal"] == false);  // an official final exists, the recording's differs
    }
}

TEST(a_failing_recording_capture_stops_starting_batches) {
    if (!commands_run()) return;
    World world;
    Run run = capture(world, "pybind-py27", {"--fail", world.ids[0], "--only-kind", "record"}, {"--all", "--workers", "1"});
    CHECK_EQ(run.outcome.code, 2);
    CHECK(contains(run.outcome.error, "recording of shot " + world.ids[0] + " failed"));
    CHECK_EQ(read_calls(run.calls).size(), static_cast<std::size_t>(2));  // the batch, then the failing shot alone
}

int main() { return testing::run_all_tests(); }
