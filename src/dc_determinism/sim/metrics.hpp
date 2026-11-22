// Shared parts of the `compare` commands: the presence rule, the board
// comparison and the statistics (percentiles with Python's operation order).
#pragma once

#include <cstdint>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"
#include "logs/log_compare.hpp"

namespace dc_determinism {

constexpr double kDisabledLimit = 1.0e-7;  // a stone whose values are all within this bound is absent
constexpr double kCentimetre = 0.01;
constexpr std::size_t kSlots = 16;

struct Threshold {
    const char* label;
    double limit;
};
extern const Threshold kThresholds[5];  // 0, 1e-6, 1e-4, 1e-3, 1e-2

// `percentile(values, percent)`: linear interpolation on the sorted floats; null when empty.
Json percentile(const std::vector<double>& values, int percent);

// `distribution(values)`: {count, max, p50, p90, p99}. `max` of floats is a float, `max` of ints
// (distribution_ints) is an int, as `max()` returns the element itself.
Json distribution(const std::vector<double>& values);
Json distribution_ints(const std::vector<std::int64_t>& values);

// `threshold_counts(values)`: {"gt_<label>": number of values > limit}.
Json threshold_counts(const std::vector<double>& values);

// `{"gt_<label>": limit}` (the `thresholds_m` block of the reports).
Json thresholds_json();

// `is_disabled(values)`: all values within 1e-7 of zero (NaN keeps a stone present).
bool is_disabled(const double* values, std::size_t count);

// `compare_boards(a, b)`: per-slot presence classes and distances of two 16-slot boards.
Json compare_boards(const Board& a, const Board& b);

// `outcome_pair(a, b, _outcome)`.
Json outcome_pair(const Board& a, const Board& b);

}  // namespace dc_determinism
