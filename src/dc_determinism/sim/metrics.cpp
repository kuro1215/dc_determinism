#include "sim/metrics.hpp"

#include <algorithm>
#include <cmath>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/pymath.hpp"

namespace dc_determinism {

const Threshold kThresholds[5] = {{"0", 0.0}, {"1e-6", 1e-6}, {"1e-4", 1e-4}, {"1e-3", 1e-3}, {"1e-2", 1e-2}};

Json percentile(const std::vector<double>& values, int percent) {
    if (values.empty()) return nullptr;
    std::vector<double> ordered = values;
    std::sort(ordered.begin(), ordered.end());
    if (ordered.size() == 1) return ordered[0];
    // Python: index = (len(ordered) - 1) * percent / 100.0 (an exact int, then a true division).
    double index = static_cast<double>(static_cast<std::int64_t>(ordered.size() - 1) * percent) / 100.0;
    double lower = std::floor(index);
    double upper = std::ceil(index);
    if (lower == upper) return ordered[static_cast<std::size_t>(lower)];
    double low = ordered[static_cast<std::size_t>(lower)];
    double high = ordered[static_cast<std::size_t>(upper)];
    return low + (high - low) * (index - lower);
}

namespace {

Json distribution_block(const std::vector<double>& values, Json maximum) {
    Json out = Json::object();
    out["count"] = static_cast<std::int64_t>(values.size());
    out["max"] = std::move(maximum);
    out["p50"] = percentile(values, 50);
    out["p90"] = percentile(values, 90);
    out["p99"] = percentile(values, 99);
    return out;
}

}  // namespace

Json distribution(const std::vector<double>& values) {
    Json maximum = nullptr;
    if (!values.empty()) {
        double best = values[0];
        for (double value : values) {
            if (value > best) best = value;  // Python max(): the first of equal maxima
        }
        maximum = best;
    }
    return distribution_block(values, std::move(maximum));
}

Json distribution_ints(const std::vector<std::int64_t>& values) {
    std::vector<double> floats;
    floats.reserve(values.size());
    for (std::int64_t value : values) floats.push_back(static_cast<double>(value));
    Json maximum = nullptr;
    if (!values.empty()) maximum = *std::max_element(values.begin(), values.end());
    return distribution_block(floats, std::move(maximum));
}

Json threshold_counts(const std::vector<double>& values) {
    Json out = Json::object();
    for (const Threshold& threshold : kThresholds) {
        std::int64_t count = 0;
        for (double value : values) count += value > threshold.limit ? 1 : 0;
        out[std::string("gt_") + threshold.label] = count;
    }
    return out;
}

Json thresholds_json() {
    Json out = Json::object();
    for (const Threshold& threshold : kThresholds) out[std::string("gt_") + threshold.label] = threshold.limit;
    return out;
}

bool is_disabled(const double* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!(std::fabs(values[i]) <= kDisabledLimit)) return false;
    }
    return true;
}

Json compare_boards(const Board& a, const Board& b) {
    std::int64_t both_present = 0, both_absent = 0, a_only = 0, b_only = 0;
    Json mismatched = Json::array();
    std::vector<std::pair<std::size_t, double>> distances;  // slot ascending
    for (std::size_t slot = 0; slot < kSlots; ++slot) {
        const Stone& left = a[slot];
        const Stone& right = b[slot];
        if (!left.present && !right.present) {
            ++both_absent;
        } else if (!right.present) {
            ++a_only;
            mismatched.push_back(static_cast<std::int64_t>(slot));
        } else if (!left.present) {
            ++b_only;
            mismatched.push_back(static_cast<std::int64_t>(slot));
        } else {
            ++both_present;
            distances.emplace_back(slot, py_hypot(left.x - right.x, left.y - right.y));
        }
    }
    Json classes = Json::object();
    classes["both_present"] = both_present;
    classes["both_absent"] = both_absent;
    classes["a_only"] = a_only;
    classes["b_only"] = b_only;

    Json out = Json::object();
    out["classes"] = classes;
    out["presence_mismatch_slots"] = mismatched;
    if (distances.empty()) {
        out["max_distance"] = nullptr;
        out["max_slot"] = nullptr;
    } else {
        std::size_t worst = 0;  // max(): the first maximal slot
        for (std::size_t i = 1; i < distances.size(); ++i) {
            if (distances[i].second > distances[worst].second) worst = i;
        }
        out["max_distance"] = distances[worst].second;
        out["max_slot"] = static_cast<std::int64_t>(distances[worst].first);
    }
    std::int64_t gt_0 = 0, gt_1cm = 0;
    Json listed = Json::object();
    for (const auto& item : distances) {
        gt_0 += item.second > 0 ? 1 : 0;
        gt_1cm += item.second > kCentimetre ? 1 : 0;
        if (item.second > 0) listed[std::to_string(item.first)] = item.second;
    }
    out["stones_gt_0"] = gt_0;
    out["stones_gt_1cm"] = gt_1cm;
    out["distances"] = listed;
    return out;
}

Json outcome_pair(const Board& a, const Board& b) {
    Json left = board_outcome(a);
    Json right = board_outcome(b);
    Json out = Json::object();
    bool no1_equal = left["no1"] == right["no1"];
    bool scores_equal = left["scores"] == right["scores"];
    bool tie_equal = left["tie"] == right["tie"];
    out["a"] = std::move(left);
    out["b"] = std::move(right);
    out["no1_equal"] = no1_equal;
    out["scores_equal"] = scores_equal;
    out["tie_equal"] = tie_equal;
    return out;
}

}  // namespace dc_determinism
