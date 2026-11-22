// `compare pybind` (Path A, the C API through our worker, against Path B, the historical official pybind
// extensions), its `--check-recording` mode and the pieces of them the unit tests reach.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "json/json.hpp"
#include "logs/log_compare.hpp"

namespace dc_determinism {
namespace cli {

int command_compare_pybind(const ParsedArgs& args);
// `compare pybind --check-recording`.
int command_check_recording(const ParsedArgs& args);

}  // namespace cli

// Tournament key of a log container path: the leading letters+digits of the first path component below the
// logs directory that has them (`gat23-<uuid>.zip` -> `gat23`, also below subdirectories); `unknown` otherwise.
// An empty `logs_dir` means no directory.
std::string tournament_of(const std::string& container, const std::string& logs_dir);

// Maps a pybind final board (`[team0, team1]`, each up to 8 `[x, y]` points) onto the 16 C API stone slots
// (`team * 8 + index`); a stone is absent when both coordinates are within 1e-7 of zero. DcDeterminismError for a bad
// shape ("incompatible final-board shape", "invalid final-board point", "non-finite final-board point").
Board pybind_board(const Json& final_board);

// The summary block of a list of per-shot entries of one engine ({status, ...} as `compare pybind` writes
// them).
Json pybind_group_summary(const std::vector<const Json*>& entries);

}  // namespace dc_determinism
