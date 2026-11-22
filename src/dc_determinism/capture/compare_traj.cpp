#include "capture/compare_traj.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <thread>

#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/pymath.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/metrics.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {

const char* const kCaptureComparisonSchema = "dc-determinism-capture-comparison-v2";

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2 * kPi;
constexpr double kProgressIntervalSeconds = 5.0;
const char* const kContractKeys[] = {"observation", "max_steps", "record_digest", "corpus_input_sha256"};
const char* const kScope = "captured common shots (not corpus-wide)";

constexpr std::size_t kPositionKind = 0, kVelocityKind = 1, kAngleKind = 2, kSpinKind = 3;
const char* const kKindNames[4] = {"position", "velocity", "angle", "w"};

double widen(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<double>(value);
}

// `wrapped_angle(a, b)`: absolute angle difference wrapped to [0, pi].
double wrapped_angle(double a, double b) {
    double difference = std::fmod(std::fabs(a - b), kTwoPi);
    return difference > kPi ? kTwoPi - difference : difference;
}

// `column_pair`: the column continued with its last value to `length` frames (zeros when the stone
// is absent or the trajectory has no frame).
std::vector<std::uint32_t> padded_column(const CapturedTrajectory& trajectory, int stone, int field,
                                         std::int64_t length) {
    std::size_t size = static_cast<std::size_t>(length);
    if (!trajectory.has_stone(stone)) return std::vector<std::uint32_t>(size, 0u);
    std::vector<std::uint32_t> bits = trajectory.column_bits(stone, field);
    if (trajectory.frames() == length) return bits;
    if (trajectory.frames() == 0) return std::vector<std::uint32_t>(size, 0u);
    std::uint32_t last = bits.back();
    bits.resize(size, last);
    return bits;
}

struct Maximum {
    double value = 0.0;
    bool has = false;
    std::int64_t step = 0;
    std::int64_t stone = 0;
};

Json maximum_json(const Maximum& maximum) {
    Json out = Json::object();
    out["value"] = maximum.value;
    out["step"] = maximum.has ? Json(maximum.step) : Json(nullptr);
    out["stone"] = maximum.has ? Json(maximum.stone) : Json(nullptr);
    return out;
}

Json digest_equal(const Json& left, const Json& right) {
    if (left.is_null() || right.is_null()) return nullptr;
    return left == right;
}

Json shot_row(const Json& row_a, const Json& row_b, const Json& metrics) {
    bool truncated = row_a["status"] == "truncated" || row_b["status"] == "truncated";
    Json out = Json::object();
    out["index"] = row_a["index"];
    out["shot_id"] = row_a["shot_id"];
    out["status_a"] = row_a["status"];
    out["status_b"] = row_b["status"];
    out["truncated"] = truncated;
    out["steps_a"] = row_a["steps"];
    out["steps_b"] = row_b["steps"];
    out["step_count_difference"] = row_b["steps"].get<std::int64_t>() - row_a["steps"].get<std::int64_t>();
    out["trajectory_digest_equal"] =
        digest_equal(json_get(row_a, "trajectory_digest"), json_get(row_b, "trajectory_digest"));
    out["final_digest_equal"] = digest_equal(json_get(row_a, "final_digest"), json_get(row_b, "final_digest"));
    for (auto it = metrics.begin(); it != metrics.end(); ++it) out[it.key()] = it.value();
    return out;
}

}  // namespace

Json compare_trajectories(const CapturedTrajectory& a, const CapturedTrajectory& b) {
    std::int64_t length = std::max(a.frames(), b.frames());
    std::set<int> stone_set(a.stones().begin(), a.stones().end());
    stone_set.insert(b.stones().begin(), b.stones().end());

    std::array<Maximum, 4> maxima;
    bool has_first_step = false;
    std::int64_t first_step = 0;
    std::map<std::int64_t, std::set<int>> first_fields;
    std::map<std::int64_t, std::set<int>> first_stones;
    std::int64_t pairs = 0;
    bool has_first_presence = false;
    std::int64_t first_presence = 0;
    std::int64_t nonfinite = 0;
    std::set<std::int64_t> differing_frames;
    std::vector<std::pair<int, double>> final_distance;
    std::vector<int> final_mismatch;
    Board board_a, board_b;

    auto keep = [&](std::size_t kind, double value, std::int64_t step, int stone) {
        if (!std::isfinite(value)) {
            ++nonfinite;
        } else if (value > maxima[kind].value) {
            maxima[kind].value = value;
            maxima[kind].has = true;
            maxima[kind].step = step;
            maxima[kind].stone = stone;
        }
    };

    for (int stone : stone_set) {
        std::array<std::vector<std::uint32_t>, kDctrFieldCount> left, right;
        std::array<std::vector<std::int64_t>, kDctrFieldCount> diffs;
        bool any = false;
        for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
            left[field] = padded_column(a, stone, static_cast<int>(field), length);
            right[field] = padded_column(b, stone, static_cast<int>(field), length);
            if (left[field] != right[field]) {
                for (std::size_t i = 0; i < left[field].size(); ++i) {
                    if (left[field][i] != right[field][i]) diffs[field].push_back(static_cast<std::int64_t>(i));
                }
            }
            any = any || !diffs[field].empty();
        }
        if (length != 0) {
            double state_a[kDctrFieldCount], state_b[kDctrFieldCount];
            for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
                state_a[field] = widen(left[field].back());
                state_b[field] = widen(right[field].back());
            }
            bool present_a = !is_disabled(state_a, kDctrFieldCount);
            bool present_b = !is_disabled(state_b, kDctrFieldCount);
            if (stone < static_cast<int>(kSlots)) {
                Stone sa, sb;
                if (present_a) sa = Stone{true, state_a[0], state_a[1]};
                if (present_b) sb = Stone{true, state_b[0], state_b[1]};
                board_a[static_cast<std::size_t>(stone)] = sa;
                board_b[static_cast<std::size_t>(stone)] = sb;
            }
            if (present_a && present_b) {
                final_distance.emplace_back(stone, py_hypot(state_a[0] - state_b[0], state_a[1] - state_b[1]));
            } else if (present_a != present_b) {
                final_mismatch.push_back(stone);
            }
        }
        if (!any) continue;
        for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
            if (diffs[field].empty()) continue;
            std::int64_t step = diffs[field][0];
            if (!has_first_step || step < first_step) {
                has_first_step = true;
                first_step = step;
            }
        }
        std::set<std::int64_t> steps;
        for (const auto& indexes : diffs) steps.insert(indexes.begin(), indexes.end());
        differing_frames.insert(steps.begin(), steps.end());
        for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
            if (!diffs[field].empty() && diffs[field][0] == first_step) {
                first_fields[first_step].insert(static_cast<int>(field));
                first_stones[first_step].insert(stone);
            }
        }
        for (std::int64_t step : steps) {
            std::size_t at = static_cast<std::size_t>(step);
            double va[kDctrFieldCount], vb[kDctrFieldCount];
            for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
                va[field] = widen(left[field][at]);
                vb[field] = widen(right[field][at]);
            }
            bool present_a = !is_disabled(va, kDctrFieldCount);
            bool present_b = !is_disabled(vb, kDctrFieldCount);
            if (present_a && present_b) {
                keep(kPositionKind, py_hypot(va[0] - vb[0], va[1] - vb[1]), step, stone);
                keep(kVelocityKind, py_hypot(va[3] - vb[3], va[4] - vb[4]), step, stone);
                keep(kAngleKind, wrapped_angle(va[2], vb[2]), step, stone);
                keep(kSpinKind, std::fabs(va[5] - vb[5]), step, stone);
            } else if (present_a != present_b) {
                ++pairs;
                if (!has_first_presence || step < first_presence) {
                    has_first_presence = true;
                    first_presence = step;
                }
            }
        }
    }

    Json first_diff = nullptr;
    if (has_first_step) {
        const std::set<int> empty;
        auto fields_found = first_fields.find(first_step);
        auto stones_found = first_stones.find(first_step);
        const std::set<int>& fields = fields_found == first_fields.end() ? empty : fields_found->second;
        const std::set<int>& stones = stones_found == first_stones.end() ? empty : stones_found->second;
        Json names = Json::array();
        for (int field : fields) names.push_back(kDctrFields[field]);
        Json listed = Json::array();
        for (int stone : stones) listed.push_back(stone);
        first_diff = Json::object();
        first_diff["step"] = first_step;
        first_diff["field"] = fields.empty() ? Json(nullptr) : Json(kDctrFields[*fields.begin()]);
        first_diff["fields"] = names;
        first_diff["stone"] = stones.empty() ? Json(nullptr) : Json(*stones.begin());
        first_diff["stones"] = listed;
    }

    Json max_diff = Json::object();
    for (std::size_t kind = 0; kind < 4; ++kind) max_diff[kKindNames[kind]] = maximum_json(maxima[kind]);

    Json distances = Json::object();
    double max_position = 0.0;
    bool have_distance = false;
    for (const auto& item : final_distance) {
        distances[std::to_string(item.first)] = item.second;
        if (!have_distance || item.second > max_position) max_position = item.second;
        have_distance = true;
    }
    Json mismatch_stones = Json::array();
    for (int stone : final_mismatch) mismatch_stones.push_back(stone);
    Json final_block = Json::object();
    final_block["position_distance"] = distances;
    final_block["max_position_distance"] = max_position;
    final_block["presence_mismatch_stones"] = mismatch_stones;

    Json presence = Json::object();
    presence["mismatch_pairs"] = pairs;
    presence["first_step"] = has_first_presence ? Json(first_presence) : Json(nullptr);

    Json result = Json::object();
    result["frames_a"] = a.frames();
    result["frames_b"] = b.frames();
    result["common_frames"] = std::min(a.frames(), b.frames());
    result["differing"] = has_first_step;
    result["differing_frames"] = static_cast<std::int64_t>(differing_frames.size());
    result["first_diff"] = first_diff;
    result["max_diff"] = max_diff;
    result["final"] = final_block;
    result["presence"] = presence;
    result["nonfinite_values"] = nonfinite;
    if (length != 0) result["outcome"] = outcome_pair(board_a, board_b);
    return result;
}

Json summarize_trajectory_rows(const Json& rows) {
    std::vector<const Json*> usable, differing;
    for (const Json& row : rows) {
        if (row["truncated"].get<bool>()) continue;
        usable.push_back(&row);
        if (row["differing"].get<bool>()) differing.push_back(&row);
    }
    std::vector<std::pair<std::string, std::int64_t>> by_field, by_set;
    auto bump = [](std::vector<std::pair<std::string, std::int64_t>>& counts, const std::string& key) {
        for (auto& item : counts) {
            if (item.first == key) {
                ++item.second;
                return;
            }
        }
        counts.emplace_back(key, 1);
    };
    std::int64_t velocity_only = 0;
    std::vector<std::int64_t> first_steps;
    for (const Json* row : differing) {
        const Json& diff = (*row)["first_diff"];
        bump(by_field, diff["field"].is_null() ? std::string("None") : diff["field"].get<std::string>());
        std::string key;
        bool only_velocity = true;
        for (const Json& field : diff["fields"]) {
            std::string name = field.get<std::string>();
            if (!key.empty()) key += "+";
            key += name;
            only_velocity = only_velocity && (name == "vx" || name == "vy" || name == "w");
        }
        bump(by_set, key);
        velocity_only += only_velocity ? 1 : 0;
        first_steps.push_back(diff["step"].get<std::int64_t>());
    }
    std::vector<double> final_values, trajectory_values, velocity_values, angle_values, spin_values;
    std::int64_t outcomes = 0;
    Json no1 = Json::array(), scores = Json::array(), truncated_ids = Json::array(), step_diff_ids = Json::array();
    std::int64_t step_diff = 0, presence_diff = 0, final_presence_diff = 0;
    for (const Json* row : usable) {
        final_values.push_back((*row)["final"]["max_position_distance"].get<double>());
        trajectory_values.push_back((*row)["max_diff"]["position"]["value"].get<double>());
        velocity_values.push_back((*row)["max_diff"]["velocity"]["value"].get<double>());
        angle_values.push_back((*row)["max_diff"]["angle"]["value"].get<double>());
        spin_values.push_back((*row)["max_diff"]["w"]["value"].get<double>());
        if (row->contains("outcome")) {
            ++outcomes;
            if (!(*row)["outcome"]["no1_equal"].get<bool>()) no1.push_back((*row)["shot_id"]);
            if (!(*row)["outcome"]["scores_equal"].get<bool>()) scores.push_back((*row)["shot_id"]);
        }
        if ((*row)["step_count_difference"].get<std::int64_t>() != 0) {
            ++step_diff;
            step_diff_ids.push_back((*row)["shot_id"]);
        }
        bool final_mismatch = !(*row)["final"]["presence_mismatch_stones"].empty();
        if (py_truthy((*row)["presence"]["mismatch_pairs"]) || final_mismatch) ++presence_diff;
        if (final_mismatch) ++final_presence_diff;
    }
    for (const Json& row : rows) {
        if (row["truncated"].get<bool>()) truncated_ids.push_back(row["shot_id"]);
    }

    Json field_counts = Json::object(), set_counts = Json::object();
    for (const auto& item : by_field) field_counts[item.first] = item.second;
    for (const auto& item : by_set) set_counts[item.first] = item.second;

    Json final_block = distribution(final_values);
    final_block["above"] = threshold_counts(final_values);
    Json trajectory_block = distribution(trajectory_values);
    trajectory_block["above"] = threshold_counts(trajectory_values);

    Json outcome = Json::object();
    outcome["compared"] = outcomes;
    outcome["no1_changed"] = static_cast<std::int64_t>(no1.size());
    outcome["scores_changed"] = static_cast<std::int64_t>(scores.size());
    outcome["no1_changed_shots"] = no1;
    outcome["scores_changed_shots"] = scores;

    Json out = Json::object();
    out["scope"] = kScope;
    out["shots"] = static_cast<std::int64_t>(rows.size());
    out["truncated"] = static_cast<std::int64_t>(rows.size() - usable.size());
    out["truncated_shots"] = truncated_ids;
    out["analysed"] = static_cast<std::int64_t>(usable.size());
    out["differing"] = static_cast<std::int64_t>(differing.size());
    out["identical"] = static_cast<std::int64_t>(usable.size() - differing.size());
    out["velocity_only_first_diff"] = velocity_only;
    out["first_diff_field_counts"] = field_counts;
    out["first_diff_field_set_counts"] = set_counts;
    out["first_diff_step"] = distribution_ints(first_steps);
    out["step_count_difference"] = step_diff;
    out["step_count_difference_shots"] = step_diff_ids;
    out["presence_difference"] = presence_diff;
    out["final_presence_difference"] = final_presence_diff;
    out["final_displacement_m"] = final_block;
    out["max_trajectory_displacement_m"] = trajectory_block;
    out["max_velocity_difference"] = distribution(velocity_values);
    out["max_angle_difference"] = distribution(angle_values);
    out["max_w_difference"] = distribution(spin_values);
    out["outcome"] = outcome;
    return out;
}

namespace {

// `digest_sections`: trajectory and final digest comparison over the common shots.
Json digest_sections(const CaptureArchive& a, const CaptureArchive& b, const std::vector<std::string>& common) {
    const struct {
        const char* mode;
        const char* key;
    } modes[] = {{"trajectory", "trajectory_digest"}, {"final", "final_digest"}};
    Json sections = Json::object();
    for (const auto& mode : modes) {
        std::int64_t compared = 0, excluded = 0;
        Json mismatches = Json::array();
        for (const std::string& id : common) {
            const Json& row_a = a.row(id);
            const Json& row_b = b.row(id);
            Json digest_a = json_get(row_a, mode.key);
            Json digest_b = json_get(row_b, mode.key);
            if (row_a["status"] != "stopped" || row_b["status"] != "stopped" || digest_a.is_null() ||
                digest_b.is_null()) {
                ++excluded;
            } else {
                ++compared;
                if (digest_a != digest_b) mismatches.push_back(id);
            }
        }
        Json section = Json::object();
        section["compared"] = compared;
        section["excluded"] = excluded;
        section["mismatches"] = static_cast<std::int64_t>(mismatches.size());
        section["mismatch_shots"] = mismatches;
        section["mismatch_rate"] =
            compared != 0 ? Json(static_cast<double>(mismatches.size()) / static_cast<double>(compared))
                          : Json(nullptr);
        sections[mode.mode] = section;
    }
    return sections;
}

// `binary_identity`: `(loader or {}).get("sha256") or (engine_binary or {}).get("sha256")`.
Json binary_identity(const CaptureArchive& archive) {
    Json provenance = json_get(archive.index(), "provenance");
    Json loader = json_get(provenance, "loader");
    Json first = json_get(py_truthy(loader) ? loader : Json::object(), "sha256");
    if (py_truthy(first)) return first;
    Json engine = json_get(provenance, "engine_binary");
    return json_get(py_truthy(engine) ? engine : Json::object(), "sha256");
}

// `without_paths`: a copy of `value` with every `path` key removed from its objects, recursively
// (objects and arrays are descended; the order of the remaining keys is kept).
Json without_paths(const Json& value) {
    if (value.is_object()) {
        Json out = Json::object();
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() != "path") out[it.key()] = without_paths(it.value());
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) out.push_back(without_paths(item));
        return out;
    }
    return value;
}

Json describe(const CaptureArchive& archive) {
    Json provenance = json_get(archive.index(), "provenance");
    Json loader_source = json_get(provenance, "loader");
    Json loader = Json::object();
    for (const char* key : {"sha256", "compiler", "platform", "origin"}) {
        loader[key] = json_get(py_truthy(loader_source) ? loader_source : Json::object(), key);
    }
    Json worker_source = json_get(provenance, "worker");
    Json worker = Json::object();
    for (const char* key : {"sha256", "version"}) {
        worker[key] = json_get(py_truthy(worker_source) ? worker_source : Json::object(), key);
    }
    Json out = Json::object();
    out["sha256"] = sha256_file(path_from_utf8(archive.path()));
    out["shots"] = static_cast<std::int64_t>(archive.shot_ids().size());
    out["engine"] = json_get(archive.contract(), "engine");
    out["loader"] = loader;
    out["engine_binary"] = without_paths(json_get(provenance, "engine_binary"));
    out["worker"] = worker;
    out["environment"] = json_get(provenance, "environment");
    return out;
}

}  // namespace

int compare_archives(const std::string& path_a, const std::string& path_b, const fs::path& output,
                     std::int64_t workers_requested) {
    refuse_existing(output);
    CaptureArchive archive_a(path_a);
    CaptureArchive archive_b(path_b);

    for (const char* key : kContractKeys) {
        Json left = json_get(archive_a.contract(), key);
        Json right = json_get(archive_b.contract(), key);
        if (left != right) {
            throw DcDeterminismError(std::string("incompatible archives: contract ") + key + " differs (" + py_repr(left) +
                             " vs " + py_repr(right) + ")");
        }
    }
    Json provenance_a = json_get(archive_a.index(), "provenance");
    Json provenance_b = json_get(archive_b.index(), "provenance");
    Json loader_a = json_get(provenance_a, "loader");
    Json loader_b = json_get(provenance_b, "loader");
    Json identity = nullptr;
    if (py_truthy(loader_a) && py_truthy(loader_b)) identity = check_source_identity(loader_a, loader_b);
    Json engine_a = json_get(archive_a.contract(), "engine");
    Json engine_b = json_get(archive_b.contract(), "engine");

    const std::vector<std::string>& ids_a = archive_a.shot_ids();
    const std::vector<std::string>& ids_b = archive_b.shot_ids();
    std::vector<std::string> common;
    Json a_only = Json::array(), b_only = Json::array(), compared_shots = Json::array();
    for (const std::string& id : ids_a) {
        if (archive_b.has(id)) {
            common.push_back(id);
            compared_shots.push_back(id);
        } else {
            a_only.push_back(id);
        }
    }
    for (const std::string& id : ids_b) {
        if (!archive_a.has(id)) b_only.push_back(id);
    }
    Json selection = Json::object();
    selection["scope"] = kScope;
    selection["a_shots"] = static_cast<std::int64_t>(ids_a.size());
    selection["b_shots"] = static_cast<std::int64_t>(ids_b.size());
    selection["compared"] = static_cast<std::int64_t>(common.size());
    selection["compared_shots"] = compared_shots;
    selection["a_only"] = a_only;
    selection["b_only"] = b_only;
    selection["status"] = common.empty() ? "empty_intersection" : "ok";
    if (common.empty()) log("warning: the archives have no shot in common");

    std::size_t total = common.size();
    std::size_t workers = total == 0 ? 0
                                     : static_cast<std::size_t>(std::max<std::int64_t>(
                                           1, std::min<std::int64_t>(workers_requested,
                                                                     static_cast<std::int64_t>(total))));
    std::vector<Json> rows(total);
    std::vector<std::string> errors(total);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> first_failed{total};
    std::atomic<std::size_t> running{workers};

    auto started = std::chrono::steady_clock::now();
    auto work = [&] {
        try {
            std::unique_ptr<ZipReader> reader_a = archive_a.open_reader();
            std::unique_ptr<ZipReader> reader_b = archive_b.open_reader();
            for (;;) {
                std::size_t i = next.fetch_add(1);
                if (i >= total || i > first_failed.load()) break;
                try {
                    const std::string& id = common[i];
                    CapturedTrajectory left = archive_a.trajectory(id, *reader_a);
                    CapturedTrajectory right = archive_b.trajectory(id, *reader_b);
                    rows[i] = shot_row(archive_a.row(id), archive_b.row(id), compare_trajectories(left, right));
                } catch (const std::exception& exc) {
                    errors[i] = exc.what();
                    std::size_t seen = first_failed.load();
                    while (i < seen && !first_failed.compare_exchange_weak(seen, i)) {
                    }
                }
                done.fetch_add(1);
            }
        } catch (const std::exception& exc) {
            // The archive could not be opened again: report it for the first shot.
            if (total != 0) {
                errors[0] = exc.what();
                first_failed.store(0);
            }
        }
        running.fetch_sub(1);
    };
    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < workers; ++t) threads.emplace_back(work);
    auto last_report = std::chrono::steady_clock::now();
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_report).count() >=
            kProgressIntervalSeconds) {
            log("compared shots " + std::to_string(done.load()) + "/" + std::to_string(total));
            last_report = std::chrono::steady_clock::now();
        }
    }
    for (std::thread& thread : threads) thread.join();
    if (first_failed.load() < total) throw DcDeterminismError(errors[first_failed.load()]);

    Json digests = digest_sections(archive_a, archive_b, common);
    Json identity_a = binary_identity(archive_a);
    Json identity_b = binary_identity(archive_b);
    Json rows_json = Json::array();
    for (Json& row : rows) rows_json.push_back(std::move(row));
    Json summary = summarize_trajectory_rows(rows_json);
    std::int64_t digest_mismatches = 0;
    for (const char* mode : {"trajectory", "final"}) digest_mismatches += digests[mode]["mismatches"].get<std::int64_t>();

    Json contract = Json::object();
    for (const char* key : kContractKeys) contract[key] = archive_a.contract()[key];
    Json engines = Json::object();
    engines["a"] = engine_a;
    engines["b"] = engine_b;
    Json inputs = Json::array();
    inputs.push_back(describe(archive_a));
    inputs.push_back(describe(archive_b));

    Json definitions = Json::object();
    definitions["step"] = "frame index = simulator steps executed (frame 0 is the initial state)";
    definitions["presence"] = "a stone is present when its six values are not all within 1e-7 of zero";
    definitions["distance"] =
        "position Euclidean distance (m); velocity Euclidean norm of the difference (m/s); angle absolute "
        "difference wrapped to [0, pi] (rad); w absolute difference (rad/s)";
    definitions["different_lengths"] = "the shorter trajectory is continued with its final state";

    bool identical = !common.empty() && digest_mismatches == 0 && summary["differing"].get<std::int64_t>() == 0 &&
                     summary["truncated"].get<std::int64_t>() == 0 &&
                     summary["presence_difference"].get<std::int64_t>() == 0 && a_only.empty() && b_only.empty();

    Json report = Json::object();
    report["schema"] = kCaptureComparisonSchema;
    report["engines"] = engines;
    report["engine_relation"] = engine_a == engine_b ? "same" : "cross";
    report["classification"] = !identity_a.is_null() && identity_a == identity_b ? "same-binary" : "cross-binary";
    report["loader_identity"] = identity;
    report["contract"] = contract;
    report["inputs"] = inputs;
    report["environment"] = environment::collect();
    report["selection"] = selection;
    report["thresholds_m"] = thresholds_json();
    report["definitions"] = definitions;
    report["wall_s"] = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    for (auto it = digests.begin(); it != digests.end(); ++it) report[it.key()] = it.value();
    report["shots"] = rows_json;
    report["summary"] = summary;
    report["identical"] = identical;

    write_json_new(output, report);
    log("compare simulator (" + report["engine_relation"].get<std::string>() + " engine, " +
        report["classification"].get<std::string>() + "): " + std::to_string(summary["shots"].get<std::int64_t>()) +
        " shots, differing " + std::to_string(summary["differing"].get<std::int64_t>()) + ", truncated " +
        std::to_string(summary["truncated"].get<std::int64_t>()) + ", identical " +
        std::to_string(summary["identical"].get<std::int64_t>()) + ", digest mismatches " +
        std::to_string(report["trajectory"]["mismatches"].get<std::int64_t>()) + "/" +
        std::to_string(report["final"]["mismatches"].get<std::int64_t>()) + "; wrote " + path_text(output));
    return identical ? 0 : 1;
}

}  // namespace dc_determinism
