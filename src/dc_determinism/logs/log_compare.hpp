// Log comparison: worker-sampled boards against the recorded shot trajectories.
//
// Everything here is pure computation. The arithmetic follows the reference formulas operation by
// operation (`math.hypot` of CPython, `round()` half to even), so that every reported number is
// the double that CPython would compute.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "json/json.hpp"

namespace dc_determinism {

constexpr std::size_t kBoardStones = 16;
constexpr std::size_t kBoardSize = 130;  // u16 presence mask + 16 x (f32 x, f32 y)

struct Stone {
    bool present = false;
    double x = 0.0;
    double y = 0.0;
};
using Board = std::array<Stone, kBoardStones>;

struct LogSettings {
    std::string basis = "trajectory-last";  // "trajectory-last" | "finish"
    bool fallback = false;
    double tolerance = 0.01;

    // {"basis", "basis_fallback", "tolerance"}
    Json to_json() const;
};

// `base64.b64decode(text, validate=True)`: false where Python raises binascii.Error.
bool base64_decode_strict(const std::string& text, std::string& out);

// --- recorded logs -------------------------------------------------------------------------

// `_board(value)`: a logged board (`team0` / `team1` lists of stones with `position`).
Board board_from_json(const Json& value);

struct Trajectory {
    std::vector<Board> frames;
    double seconds_per_frame = 0.0;
};

// `_trajectory(log)`: the board after each frame's updates, and the frame duration. Throws
// DcDeterminismError for a malformed log.
Trajectory trajectory_from_log(const Json& log);

// --- sampled boards --------------------------------------------------------------------------

// One 130-byte board at `offset` of `data` (absent stones stay not present). Throws DcDeterminismError for
// a non-finite coordinate.
Board decode_board(const std::string& data, std::size_t offset = 0);

// The final board followed by `count` boards of the requested steps.
std::pair<Board, std::vector<Board>> decode_payload(const std::string& payload, std::size_t count);

// `decode_s_line`: parses `S <pos> <n> <base64>`; returns the count and the raw payload.
std::pair<std::int64_t, std::string> decode_s_line(const std::string& line, std::int64_t position);

// --- step mapping ----------------------------------------------------------------------------

// `round((index + 1) * duration / sim_spf)` for every recorded frame.
std::vector<std::int64_t> frame_steps(std::size_t frames, double duration, double sim_spf);
// Strictly ascending distinct steps to ask the worker for.
std::vector<std::int64_t> requested_steps(const std::vector<std::int64_t>& steps);

// --- metrics ---------------------------------------------------------------------------------

// `_outcome(board)`: {"no1", "tie", "scores"}.
Json board_outcome(const Board& board);
// `_board_comparison(actual, expected, tolerance)`.
Json board_comparison(const Board& actual, const Board& expected, double tolerance);

// Sampled boards by simulator step.
using SampledBoards = std::map<std::int64_t, Board>;

// `compare_trajectory`: one comparison per log frame, even for equal steps.
Json compare_trajectory(const std::vector<Board>& recorded, const std::vector<std::int64_t>& steps,
                        const SampledBoards& sampled, double tolerance);

// `final_comparisons`: {"trajectory_last", "finish", "finish_status"} of one shot. `log` is the
// shot's `log` object (only `log["trajectory"]["finish"]` is read, and only when `fgz` is
// "none_detected"); `finish` is null for a shot with a free-guard-zone foul and `finish_status`
// names the FGZ class.
Json final_comparisons(const Json& log, const std::string& fgz, const Board& final,
                       const std::vector<Board>& recorded, double tolerance);

// `shot_metrics`: the `log` object of one shot.
Json shot_metrics(const Json& log, const std::string& fgz, const Board& final,
                  const std::vector<Board>& recorded, const std::vector<std::int64_t>& steps,
                  const SampledBoards& sampled, double tolerance);

// `entry_fields`: {"log", "primary_basis", "matched_basis"} of a reproducible shot.
Json entry_fields(const Json& metrics, const LogSettings& settings);

// `attach_log_entries`: adds the log fields in place to the rows that are reproducible and stopped
// and have metrics (a null entry of `metrics` has none).
void attach_log_entries(Json& rows, const std::vector<Json>& metrics, const LogSettings& settings);

// `log_summary`: the `log_comparison` block (without the tool identity).
Json log_summary(const Json& rows, const LogSettings& settings, const std::string& sim_bits, double sim_spf);

}  // namespace dc_determinism
