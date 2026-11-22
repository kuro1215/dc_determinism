#include "logs/log_compare.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/error.hpp"
#include "common/pymath.hpp"
#include "common/text.hpp"
#include "corpus/pytext.hpp"

namespace dc_determinism {

namespace {

[[noreturn]] void bad_s_line(std::int64_t position, const std::string& text) {
    throw DcDeterminismError("unexpected sampled-board line for position " + std::to_string(position) + ": " +
                     py_repr(Json(head_code_points(text, 80))));
}

}  // namespace

// Strict base64 (`base64.b64decode(s, validate=True)`): the standard alphabet, padding only at the
// end and complete.
bool base64_decode_strict(const std::string& text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) return false;
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::size_t padding = 0;
    while (padding < text.size() && text[text.size() - 1 - padding] == '=') ++padding;
    if (padding > 2) return false;
    std::size_t data = text.size() - padding;
    for (std::size_t i = 0; i < data; ++i) {
        if (value(text[i]) < 0) return false;
    }
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < data; i += 4) {
        std::uint32_t block = 0;
        std::size_t take = std::min<std::size_t>(4, data - i);
        for (std::size_t k = 0; k < 4; ++k) {
            block = (block << 6) | static_cast<std::uint32_t>(k < take ? value(text[i + k]) : 0);
        }
        out.push_back(static_cast<char>((block >> 16) & 0xFF));
        if (take > 2) out.push_back(static_cast<char>((block >> 8) & 0xFF));
        if (take > 3) out.push_back(static_cast<char>(block & 0xFF));
    }
    return true;
}

namespace {

const Json& member(const Json& object, const char* key) {
    if (!object.is_object()) throw DcDeterminismError(std::string("invalid log: expected an object holding '") + key + "'");
    auto found = object.find(key);
    if (found == object.end()) throw DcDeterminismError(std::string("invalid log: missing '") + key + "'");
    return *found;
}

double as_float(const Json& value, const char* what) {
    double out = 0.0;
    std::string reason;
    if (!py_float(value, out, reason)) throw DcDeterminismError(std::string("invalid log ") + what + ": " + reason);
    return out;
}

// `_position(value)`
Stone logged_position(const Json& value) {
    const Json& point = member(value, "position");
    Stone stone;
    stone.x = as_float(member(point, "x"), "position");
    stone.y = as_float(member(point, "y"), "position");
    if (!std::isfinite(stone.x) || !std::isfinite(stone.y)) throw DcDeterminismError("non-finite logged position");
    stone.present = true;
    return stone;
}

double abs_diff(double a, double b) { return std::fabs(a - b); }

}  // namespace

Json LogSettings::to_json() const {
    Json out = Json::object();
    out["basis"] = basis;
    out["basis_fallback"] = fallback;
    out["tolerance"] = tolerance;
    return out;
}

// ---------------------------------------------------------------------------------- recorded logs

Board board_from_json(const Json& value) {
    Board board;
    for (int team = 0; team < 2; ++team) {
        const Json& stones = member(value, team == 0 ? "team0" : "team1");
        if (!stones.is_array() || stones.size() > 8) throw DcDeterminismError("invalid logged board");
        for (std::size_t index = 0; index < stones.size(); ++index) {
            if (!stones[index].is_null()) board[static_cast<std::size_t>(team) * 8 + index] = logged_position(stones[index]);
        }
    }
    return board;
}

Trajectory trajectory_from_log(const Json& log) {
    const Json& trajectory = member(log, "trajectory");
    Board board = board_from_json(member(trajectory, "start"));
    Trajectory out;
    const Json& frames = member(trajectory, "frames");
    if (!frames.is_array()) throw DcDeterminismError("invalid log: trajectory frames are not a list");
    out.frames.reserve(frames.size());
    for (const Json& updates : frames) {
        if (!updates.is_array()) throw DcDeterminismError("invalid log: frame updates are not a list");
        for (const Json& update : updates) {
            if (update.is_null()) continue;
            // `sid_of(update)`
            const Json& team = member(update, "team");
            std::int64_t index = 0;
            std::string reason;
            if (!py_int(member(update, "index"), index, reason)) {
                throw DcDeterminismError("invalid logged stone slot: " + reason);
            }
            std::int64_t sid = (team.is_string() && team.get<std::string>() == "team0" ? 0 : 8) + index;
            if (sid < 0 || sid >= static_cast<std::int64_t>(kBoardStones)) throw DcDeterminismError("invalid logged stone slot");
            const Json& value = member(update, "value");
            board[static_cast<std::size_t>(sid)] = value.is_null() ? Stone() : logged_position(value);
        }
        out.frames.push_back(board);
    }
    double dt = as_float(member(trajectory, "seconds_per_frame"), "frame duration");
    if (!std::isfinite(dt) || dt <= 0) throw DcDeterminismError("invalid log frame duration");
    out.seconds_per_frame = dt;
    return out;
}

// ---------------------------------------------------------------------------------- sampled boards

Board decode_board(const std::string& data, std::size_t offset) {
    if (data.size() < offset + kBoardSize) throw DcDeterminismError("sampled-board payload is too short");
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data.data()) + offset;
    unsigned mask = static_cast<unsigned>(bytes[0]) | (static_cast<unsigned>(bytes[1]) << 8);
    Board board;
    for (std::size_t sid = 0; sid < kBoardStones; ++sid) {
        if (((mask >> sid) & 1u) == 0) continue;
        float coords[2];
        for (std::size_t axis = 0; axis < 2; ++axis) {
            const unsigned char* p = bytes + 2 + 4 * (2 * sid + axis);
            std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                 (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
            std::memcpy(&coords[axis], &bits, sizeof(float));
        }
        double x = static_cast<double>(coords[0]);
        double y = static_cast<double>(coords[1]);
        if (!std::isfinite(x) || !std::isfinite(y)) throw DcDeterminismError("worker returned a non-finite stone position");
        board[sid].present = true;
        board[sid].x = x;
        board[sid].y = y;
    }
    return board;
}

std::pair<Board, std::vector<Board>> decode_payload(const std::string& payload, std::size_t count) {
    if (payload.size() != kBoardSize * (1 + count)) {
        throw DcDeterminismError("sampled-board payload has " + std::to_string(payload.size()) + " bytes, expected " +
                         std::to_string(kBoardSize * (1 + count)));
    }
    Board final = decode_board(payload, 0);
    std::vector<Board> boards;
    boards.reserve(count);
    for (std::size_t i = 1; i <= count; ++i) boards.push_back(decode_board(payload, kBoardSize * i));
    return {final, std::move(boards)};
}

std::pair<std::int64_t, std::string> decode_s_line(const std::string& line, std::int64_t position) {
    std::string text = py_strip(line);
    if (text.compare(0, 3, "ERR") == 0) throw DcDeterminismError("worker reported: " + text);
    std::vector<std::string> fields = py_split(text);
    if (fields.size() != 4 || fields[0] != "S") bad_s_line(position, text);
    std::int64_t index = 0;
    std::int64_t count = 0;
    std::string payload;
    if (!py_parse_int(fields[1], index) || index != position || !py_parse_int(fields[2], count) || count < 0 ||
        !base64_decode_strict(fields[3], payload)) {
        bad_s_line(position, text);
    }
    return {count, std::move(payload)};
}

// ---------------------------------------------------------------------------------- step mapping

std::vector<std::int64_t> frame_steps(std::size_t frames, double duration, double sim_spf) {
    std::vector<std::int64_t> steps;
    steps.reserve(frames);
    for (std::size_t index = 0; index < frames; ++index) {
        // Python: round((index + 1) * duration / sim_spf), evaluated left to right.
        steps.push_back(py_round(static_cast<double>(index + 1) * duration / sim_spf));
    }
    return steps;
}

std::vector<std::int64_t> requested_steps(const std::vector<std::int64_t>& steps) {
    std::vector<std::int64_t> out = steps;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// ---------------------------------------------------------------------------------- metrics

Json board_outcome(const Board& board) {
    std::vector<std::pair<double, std::int64_t>> ordered;
    for (std::size_t sid = 0; sid < kBoardStones; ++sid) {
        if (!board[sid].present) continue;
        ordered.emplace_back(py_hypot(board[sid].x, board[sid].y - 38.405), static_cast<std::int64_t>(sid));
    }
    std::sort(ordered.begin(), ordered.end());
    std::vector<std::pair<double, std::int64_t>> inside;
    for (const auto& item : ordered) {
        if (item.first <= 1.974) inside.push_back(item);
    }
    Json out = Json::object();
    if (inside.empty()) {
        out["no1"] = nullptr;
        out["tie"] = false;
        out["scores"] = Json::array({0, 0});
        return out;
    }
    double distance = inside[0].first;
    std::int64_t no1 = inside[0].second;
    std::int64_t team = no1 / 8;
    std::int64_t score = 0;
    for (const auto& item : inside) {
        if (item.second / 8 != team) break;
        ++score;
    }
    std::int64_t scores[2] = {0, 0};
    scores[team] = score;
    std::int64_t equal = 0;
    for (const auto& item : inside) equal += item.first == distance ? 1 : 0;
    out["no1"] = no1;
    out["tie"] = equal > 1;
    out["scores"] = Json::array({scores[0], scores[1]});
    return out;
}

Json board_comparison(const Board& actual, const Board& expected, double tolerance) {
    std::int64_t presence = 0;
    double maximum = 0.0;
    std::int64_t compared = 0;
    for (std::size_t sid = 0; sid < kBoardStones; ++sid) {
        const Stone& left = actual[sid];
        const Stone& right = expected[sid];
        if (!left.present || !right.present) {
            presence += left.present != right.present ? 1 : 0;
            continue;
        }
        // Python: max(maximum, abs(left[0] - right[0]), abs(left[1] - right[1]))
        double dx = abs_diff(left.x, right.x);
        double dy = abs_diff(left.y, right.y);
        double best = maximum;
        if (dx > best) best = dx;
        if (dy > best) best = dy;
        maximum = best;
        ++compared;
    }
    Json actual_outcome = board_outcome(actual);
    Json expected_outcome = board_outcome(expected);
    Json out = Json::object();
    out["passed"] = presence == 0 && maximum <= tolerance;
    out["max_axis_error"] = maximum;
    out["presence_mismatches"] = presence;
    out["compared_stones"] = compared;
    bool no1_equal = actual_outcome["no1"] == expected_outcome["no1"];
    bool scores_equal = actual_outcome["scores"] == expected_outcome["scores"];
    out["actual_outcome"] = std::move(actual_outcome);
    out["expected_outcome"] = std::move(expected_outcome);
    out["no1_equal"] = no1_equal;
    out["scores_equal"] = scores_equal;
    return out;
}

Json compare_trajectory(const std::vector<Board>& recorded, const std::vector<std::int64_t>& steps,
                        const SampledBoards& sampled, double tolerance) {
    std::int64_t compared = 0;
    std::int64_t missing = 0;
    std::int64_t presence = 0;
    double max_axis = 0.0;
    std::size_t pairs = std::min(recorded.size(), steps.size());
    for (std::size_t i = 0; i < pairs; ++i) {
        auto found = sampled.find(steps[i]);
        if (found == sampled.end()) {
            ++missing;
            continue;
        }
        Json comparison = board_comparison(found->second, recorded[i], tolerance);
        double axis = comparison["max_axis_error"].get<double>();
        if (axis > max_axis) max_axis = axis;
        presence += comparison["presence_mismatches"].get<std::int64_t>();
        ++compared;
    }
    Json out = Json::object();
    out["compared_frames"] = compared;
    out["missing_frames"] = missing;
    out["presence_mismatches"] = presence;
    out["max_axis_error"] = max_axis;
    return out;
}

Json final_comparisons(const Json& log, const std::string& fgz, const Board& final,
                       const std::vector<Board>& recorded, double tolerance) {
    bool comparable = fgz == "none_detected";
    Json out = Json::object();
    out["trajectory_last"] = recorded.empty() ? Json(nullptr) : board_comparison(final, recorded.back(), tolerance);
    if (comparable) {
        const Json& trajectory = member(log, "trajectory");
        out["finish"] = board_comparison(final, board_from_json(member(trajectory, "finish")), tolerance);
    } else {
        out["finish"] = nullptr;
    }
    out["finish_status"] = comparable ? std::string("comparable") : fgz;
    return out;
}

Json shot_metrics(const Json& log, const std::string& fgz, const Board& final,
                  const std::vector<Board>& recorded, const std::vector<std::int64_t>& steps,
                  const SampledBoards& sampled, double tolerance) {
    Json out = final_comparisons(log, fgz, final, recorded, tolerance);
    out["trajectory"] = compare_trajectory(recorded, steps, sampled, tolerance);
    return out;
}

namespace {

std::string basis_key(const std::string& basis) {
    std::string key = basis;
    std::replace(key.begin(), key.end(), '-', '_');
    return key;
}

bool passed(const Json& comparison) { return comparison.is_object() && comparison["passed"].get<bool>(); }

}  // namespace

Json entry_fields(const Json& metrics, const LogSettings& settings) {
    Json entry = Json::object();
    entry["log"] = metrics;
    entry["primary_basis"] = settings.basis;
    bool matched = passed(metrics[basis_key(settings.basis)]);
    entry["matched_basis"] = matched ? Json(settings.basis) : Json(nullptr);
    if (settings.fallback && !matched) {
        std::string other = settings.basis == "trajectory-last" ? "finish" : "trajectory-last";
        if (passed(metrics[basis_key(other)])) entry["matched_basis"] = other;
    }
    return entry;
}

void attach_log_entries(Json& rows, const std::vector<Json>& metrics, const LogSettings& settings) {
    std::size_t count = std::min(rows.size(), metrics.size());
    for (std::size_t i = 0; i < count; ++i) {
        Json& row = rows[i];
        if (metrics[i].is_null()) continue;
        if (!(row["reproducible_trajectory"].get<bool>() && row["reproducible_final"].get<bool>() &&
              row["all_stopped"].get<bool>())) {
            continue;
        }
        Json entry = entry_fields(metrics[i], settings);
        for (auto it = entry.begin(); it != entry.end(); ++it) row[it.key()] = it.value();
    }
}

Json log_summary(const Json& rows, const LogSettings& settings, const std::string& sim_bits, double sim_spf) {
    std::int64_t compared = 0;
    std::int64_t trajectory_last = 0;
    std::int64_t finish = 0;
    std::int64_t unmatched = 0;
    std::int64_t finish_comparable = 0;
    for (const Json& row : rows) {
        if (!row.contains("log")) continue;
        ++compared;
        const Json& matched = row["matched_basis"];
        if (matched.is_null()) {
            ++unmatched;
        } else if (matched == "trajectory-last") {
            ++trajectory_last;
        } else {
            ++finish;
        }
        if (!row["log"]["finish"].is_null()) ++finish_comparable;
    }
    Json out = settings.to_json();
    out["simulator_seconds_per_frame_bits"] = sim_bits;
    out["simulator_seconds_per_frame"] = sim_spf;
    out["shots"] = static_cast<std::int64_t>(rows.size());
    out["compared_shots"] = compared;
    out["not_compared_shots"] = static_cast<std::int64_t>(rows.size()) - compared;
    Json by_basis = Json::object();
    by_basis["trajectory-last"] = trajectory_last;
    by_basis["finish"] = finish;
    by_basis["unmatched"] = unmatched;
    out["matched_by_basis"] = by_basis;
    out["finish_comparable"] = finish_comparable;
    return out;
}

}  // namespace dc_determinism
