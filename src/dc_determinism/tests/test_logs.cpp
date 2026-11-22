// Unit tests of the log comparison (logs/log_compare, common/pymath).
#include <cmath>
#include <cstring>
#include <limits>

#include "common/error.hpp"
#include "common/pymath.hpp"
#include "json/writer.hpp"
#include "logs/log_compare.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

Board board_with(std::initializer_list<std::pair<int, std::pair<double, double>>> stones) {
    Board board;
    for (const auto& stone : stones) {
        board[static_cast<std::size_t>(stone.first)] = Stone{true, stone.second.first, stone.second.second};
    }
    return board;
}

std::string encoded_board(unsigned mask, std::initializer_list<std::pair<int, std::pair<float, float>>> stones) {
    std::string out(kBoardSize, '\0');
    out[0] = static_cast<char>(mask & 0xFF);
    out[1] = static_cast<char>((mask >> 8) & 0xFF);
    for (const auto& stone : stones) {
        std::uint32_t bits[2];
        std::memcpy(&bits[0], &stone.second.first, 4);
        std::memcpy(&bits[1], &stone.second.second, 4);
        for (int axis = 0; axis < 2; ++axis) {
            for (int b = 0; b < 4; ++b) {
                out[2 + 4 * (2 * static_cast<std::size_t>(stone.first) + static_cast<std::size_t>(axis)) +
                    static_cast<std::size_t>(b)] = static_cast<char>((bits[axis] >> (8 * b)) & 0xFF);
            }
        }
    }
    return out;
}

}  // namespace

TEST(hypot_basic_values) {
    CHECK_EQ(py_hypot(3.0, 4.0), 5.0);
    CHECK_EQ(py_hypot(-3.0, 4.0), 5.0);
    CHECK_EQ(py_hypot(0.0, 0.0), 0.0);
    CHECK_EQ(py_hypot(0.0, -2.5), 2.5);
    CHECK(std::isinf(py_hypot(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN())));
    CHECK(std::isnan(py_hypot(1.0, std::numeric_limits<double>::quiet_NaN())));
    CHECK_EQ(py_hypot(1e-320, 0.0), 1e-320);        // subnormal scaling path
    CHECK_EQ(py_hypot(1e308, 1e308) > 1e308, true);  // finite, large
}

TEST(round_half_even) {
    CHECK_EQ(py_round(0.5), 0);
    CHECK_EQ(py_round(1.5), 2);
    CHECK_EQ(py_round(2.5), 2);
    CHECK_EQ(py_round(-0.5), 0);
    CHECK_EQ(py_round(-2.5), -2);
    CHECK_EQ(py_round(3.4999999999999996), 3);
    CHECK_EQ(py_round(1e15 + 0.5), 1000000000000000);
    CHECK_THROWS(py_round(std::numeric_limits<double>::quiet_NaN()));
    CHECK_THROWS(py_round(std::numeric_limits<double>::infinity()));
    CHECK_THROWS(py_round(1e30));
}

TEST(frame_steps_and_requests) {
    std::vector<std::int64_t> steps = frame_steps(4, 0.5, 1.0);
    CHECK_EQ(steps.size(), static_cast<std::size_t>(4));
    CHECK_EQ(steps[0], 0);
    CHECK_EQ(steps[1], 1);
    CHECK_EQ(steps[2], 2);
    CHECK_EQ(steps[3], 2);
    std::vector<std::int64_t> requested = requested_steps(steps);
    CHECK_EQ(requested.size(), static_cast<std::size_t>(3));
    CHECK_EQ(requested[2], 2);
    CHECK_EQ(frame_steps(0, 0.5, 1.0).size(), static_cast<std::size_t>(0));
}

TEST(sampled_line_decoding) {
    std::pair<std::int64_t, std::string> parsed = decode_s_line("S 7 2 YWJj\r\n", 7);
    CHECK_EQ(parsed.first, 2);
    CHECK_EQ(parsed.second, std::string("abc"));
    CHECK_EQ(decode_s_line("S 0 0 QQ==", 0).second, std::string("A"));
    CHECK_THROWS(decode_s_line("S 7 2 YWJj", 8));         // another position
    CHECK_THROWS(decode_s_line("S 7 -1 YWJj", 7));        // negative count
    CHECK_THROWS(decode_s_line("S 7 2 YWJ", 7));          // incomplete padding
    CHECK_THROWS(decode_s_line("S 7 2 Y===", 7));         // too much padding
    CHECK_THROWS(decode_s_line("S 7 2 YW!j", 7));         // character outside the alphabet
    CHECK_THROWS(decode_s_line("S 7 2 YWJj extra", 7));   // wrong field count
    CHECK_THROWS(decode_s_line("T 7 2 YWJj", 7));
    CHECK_THROWS(decode_s_line("ERR boom", 7));
}

TEST(board_decoding) {
    std::string data = encoded_board(0x8005, {{0, {1.5f, -2.0f}}, {2, {0.25f, 38.5f}}, {15, {-3.0f, 4.0f}}});
    Board board = decode_board(data);
    CHECK(board[0].present);
    CHECK_EQ(board[0].x, 1.5);
    CHECK_EQ(board[0].y, -2.0);
    CHECK(!board[1].present);
    CHECK(board[2].present);
    CHECK_EQ(board[2].y, 38.5);
    CHECK(board[15].present);
    CHECK_EQ(board[15].x, -3.0);
    std::pair<Board, std::vector<Board>> payload = decode_payload(data + data, 1);
    CHECK(payload.first[15].present);
    CHECK_EQ(payload.second.size(), static_cast<std::size_t>(1));
    CHECK_THROWS(decode_payload(data, 1));
    // a non-finite coordinate of a present stone is an error, of an absent one is ignored
    std::string bad = encoded_board(0x0001, {{0, {std::numeric_limits<float>::infinity(), 0.0f}}});
    CHECK_THROWS(decode_board(bad));
    std::string ignored = encoded_board(0x0000, {{0, {std::numeric_limits<float>::infinity(), 0.0f}}});
    CHECK(!decode_board(ignored)[0].present);
}

TEST(outcome_scores_and_ties) {
    Json empty = board_outcome(Board());
    CHECK(empty["no1"].is_null());
    CHECK_EQ(empty["tie"].get<bool>(), false);
    CHECK_EQ(json_dumps(empty["scores"], DumpOptions::compact()), std::string("[0,0]"));

    // team1 holds the two closest stones; team0 is farther: team1 scores 2
    Board board = board_with({{0, {0.5, 38.405}}, {8, {0.1, 38.405}}, {9, {0.2, 38.405}}});
    Json outcome = board_outcome(board);
    CHECK_EQ(outcome["no1"].get<int>(), 8);
    CHECK_EQ(json_dumps(outcome["scores"], DumpOptions::compact()), std::string("[0,2]"));
    CHECK_EQ(outcome["tie"].get<bool>(), false);

    // equal distances give a tie, lowest slot first
    Board tie = board_with({{3, {0.3, 38.405}}, {11, {-0.3, 38.405}}});
    Json tied = board_outcome(tie);
    CHECK_EQ(tied["no1"].get<int>(), 3);
    CHECK_EQ(tied["tie"].get<bool>(), true);
    CHECK_EQ(json_dumps(tied["scores"], DumpOptions::compact()), std::string("[1,0]"));

    // outside the house (1.974) nothing counts
    Board far = board_with({{0, {0.0, 38.405 + 1.975}}});
    CHECK(board_outcome(far)["no1"].is_null());
}

TEST(board_comparison_metrics) {
    Board a = board_with({{0, {0.0, 38.405}}, {8, {1.0, 38.405}}});
    Board b = board_with({{0, {0.005, 38.405}}, {8, {1.0, 38.4}}});
    Json same = board_comparison(a, b, 0.01);
    CHECK_EQ(same["passed"].get<bool>(), true);
    CHECK_EQ(same["compared_stones"].get<int>(), 2);
    CHECK_EQ(same["presence_mismatches"].get<int>(), 0);
    CHECK_EQ(same["no1_equal"].get<bool>(), true);
    Json strict = board_comparison(a, b, 0.001);
    CHECK_EQ(strict["passed"].get<bool>(), false);

    Board missing = board_with({{0, {0.0, 38.405}}});
    Json presence = board_comparison(a, missing, 1.0);
    CHECK_EQ(presence["passed"].get<bool>(), false);
    CHECK_EQ(presence["presence_mismatches"].get<int>(), 1);
    CHECK_EQ(presence["compared_stones"].get<int>(), 1);
    // keys follow insertion order
    std::string keys;
    for (auto it = presence.begin(); it != presence.end(); ++it) keys += it.key() + ",";
    CHECK_EQ(keys, std::string("passed,max_axis_error,presence_mismatches,compared_stones,actual_outcome,"
                               "expected_outcome,no1_equal,scores_equal,"));
}

TEST(trajectory_from_log_document) {
    Json log = parse_json_text(
        R"({"trajectory": {"start": {"team0": [{"position": {"x": 0, "y": 1.5}}, null], "team1": []},
            "frames": [[{"team": "team0", "index": 1, "value": {"position": {"x": 2.0, "y": 3.0}}}], [],
                       [null, {"team": "team1", "index": 0, "value": {"position": {"x": 9, "y": 8}}},
                        {"team": "team0", "index": 0, "value": null}]],
            "seconds_per_frame": 0.25, "finish": {"team0": [], "team1": []}}})",
        "test");
    Trajectory trajectory = trajectory_from_log(log);
    CHECK_EQ(trajectory.frames.size(), static_cast<std::size_t>(3));
    CHECK_EQ(trajectory.seconds_per_frame, 0.25);
    CHECK(trajectory.frames[0][0].present);
    CHECK(trajectory.frames[0][1].present);
    CHECK_EQ(trajectory.frames[0][1].x, 2.0);
    CHECK(trajectory.frames[1][1].present);
    CHECK(!trajectory.frames[2][0].present);
    CHECK(trajectory.frames[2][8].present);
    CHECK_EQ(trajectory.frames[2][8].y, 8.0);

    Json bad_slot = log;
    bad_slot["trajectory"]["frames"][0][0]["index"] = 16;
    CHECK_THROWS(trajectory_from_log(bad_slot));
    Json bad_duration = log;
    bad_duration["trajectory"]["seconds_per_frame"] = 0;
    CHECK_THROWS(trajectory_from_log(bad_duration));
}

TEST(final_comparisons_guard) {
    Json log = parse_json_text(R"({"trajectory": {"finish": {"team0": [{"position": {"x": 0, "y": 38.405}}], "team1": []}}})",
                               "test");
    Board final = board_with({{0, {0.0, 38.405}}});
    std::vector<Board> recorded{final};
    Json comparable = final_comparisons(log, "none_detected", final, recorded, 0.01);
    CHECK_EQ(comparable["finish_status"].get<std::string>(), std::string("comparable"));
    CHECK(comparable["finish"]["passed"].get<bool>());
    Json foul = final_comparisons(log, "rule_restore", final, recorded, 0.01);
    CHECK(foul["finish"].is_null());
    CHECK_EQ(foul["finish_status"].get<std::string>(), std::string("rule_restore"));
    Json none = final_comparisons(Json::object(), "unknown", final, std::vector<Board>(), 0.01);
    CHECK(none["trajectory_last"].is_null());
    CHECK_EQ(none["finish_status"].get<std::string>(), std::string("unknown"));
}

TEST(entry_fields_and_summary) {
    auto comparison = [](bool passed) {
        Json value = Json::object();
        value["passed"] = passed;
        return value;
    };
    Json metrics = Json::object();
    metrics["trajectory_last"] = comparison(false);
    metrics["finish"] = comparison(true);
    metrics["finish_status"] = "comparable";
    LogSettings plain;
    Json entry = entry_fields(metrics, plain);
    CHECK(entry["matched_basis"].is_null());
    CHECK_EQ(entry["primary_basis"].get<std::string>(), std::string("trajectory-last"));
    LogSettings fallback;
    fallback.fallback = true;
    CHECK_EQ(entry_fields(metrics, fallback)["matched_basis"].get<std::string>(), std::string("finish"));
    LogSettings finish_first;
    finish_first.basis = "finish";
    CHECK_EQ(entry_fields(metrics, finish_first)["matched_basis"].get<std::string>(), std::string("finish"));
    Json none_finish = metrics;
    none_finish["finish"] = nullptr;
    CHECK(entry_fields(none_finish, finish_first)["matched_basis"].is_null());

    Json rows = Json::array();
    for (int i = 0; i < 3; ++i) {
        Json row = Json::object();
        row["reproducible_trajectory"] = true;
        row["reproducible_final"] = true;
        row["all_stopped"] = i != 2;
        rows.push_back(row);
    }
    std::vector<Json> all{metrics, Json(nullptr), metrics};
    attach_log_entries(rows, all, fallback);
    CHECK(rows[0].contains("log"));
    CHECK(!rows[1].contains("log"));   // no metrics
    CHECK(!rows[2].contains("log"));   // not stopped
    Json summary = log_summary(rows, fallback, "3a83126f", 0.001);
    CHECK_EQ(summary["shots"].get<int>(), 3);
    CHECK_EQ(summary["compared_shots"].get<int>(), 1);
    CHECK_EQ(summary["not_compared_shots"].get<int>(), 2);
    CHECK_EQ(summary["matched_by_basis"]["finish"].get<int>(), 1);
    CHECK_EQ(summary["finish_comparable"].get<int>(), 1);
    CHECK_EQ(summary["basis_fallback"].get<bool>(), true);
}

int main() { return testing::run_all_tests(); }
