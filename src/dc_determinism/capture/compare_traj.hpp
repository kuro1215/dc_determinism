// `compare simulator A.zip B.zip`: digest comparison and trajectory
// metrics of two capture archives. The engines may differ (fcv1 against pybind-py27, ...) as well
// as the environments; the report labels the `engine_relation`.
//
// Rules: a stone is present in a frame when its six values are not all within 1e-7 of zero;
// distances and differences are taken only where it is present on both sides; presence mismatches
// are counted as (frame, stone) pairs; a shorter trajectory is continued with its own final state
// when compared with a longer one; shots truncated on either side are reported separately and
// kept out of the distributions.
#pragma once

#include <cstdint>
#include <string>

#include "capture/archive.hpp"
#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kCaptureComparisonSchema;  // "dc-determinism-capture-comparison-v2"

// `compare_trajectories(a, b, outcome)`: bit-level and metric comparison of two trajectories of
// the same shot (the metrics object of one shot row, with `outcome`).
Json compare_trajectories(const CapturedTrajectory& a, const CapturedTrajectory& b);

// The per-shot summary of a list of shot rows.
Json summarize_trajectory_rows(const Json& rows);

// The command: writes the report to `output` (never overwritten); returns the exit
// code (0: identical, 1: not).
int compare_archives(const std::string& path_a, const std::string& path_b, const fs::path& output,
                     std::int64_t workers);

}  // namespace dc_determinism
