// `compare engines`: FCV1 against FCV1Plus stepped in lockstep by one
// worker (`L` requests).
//
// The worker answers `L <pos> <status_a> <steps_a> <status_b> <steps_b> <first_bit_diff_step>
// <first_gt_1e-6> <first_gt_1e-3> <first_gt_1e-2> <max_distance> <max_step> <max_stone>
// <presence_mismatch_pairs> <first_presence_mismatch_step> <base64(final_a + final_b)>` with
// A = FCV1, B = FCV1Plus; -1 means "never". The per-step metrics come from the worker; the final
// boards are compared here, and optionally both are compared with the recorded game logs.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "cli/commands/capture.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "corpus/prepared.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"
#include "logs/log_compare.hpp"
#include "sim/metrics.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {
namespace cli {

namespace {

const char* const kSchema = "dc-determinism-engine-deviation-v1";
const char* const kObservation = "raw-stonecoordinate-v1";
const char* const kEngines[2] = {"fcv1", "fcv1plus"};
const char* const kTrajectoryThresholds[3] = {"1e-6", "1e-3", "1e-2"};  // metres, as the worker reports them
const char* const kPresenceClasses[4] = {"both_present", "both_absent", "a_only", "b_only"};
constexpr std::size_t kLFields = 16;
constexpr double kProgressIntervalSeconds = 5.0;

using Clock = std::chrono::steady_clock;

bool given(const ParsedArgs& args, const char* dest) { return args.has(dest) && !args.text_or(dest, "").empty(); }

// One decoded `L` answer.
struct Lockstep {
    std::string status_a, status_b;
    std::int64_t steps_a = 0, steps_b = 0;
    std::int64_t first_bit_diff_step = 0;
    std::int64_t first_gt[3] = {0, 0, 0};  // 1e-6, 1e-3, 1e-2
    double max_distance = 0.0;
    std::int64_t max_step = 0, max_stone = 0;
    std::int64_t presence_mismatch_pairs = 0, first_presence_mismatch_step = 0;
    Board board_a, board_b;
};

// `_int(text, minimum)`.
bool int_at_least(const std::string& text, std::int64_t minimum, std::int64_t& out) {
    return py_parse_int(text, out) && out >= minimum;
}

// `parse_l_line`.
Lockstep parse_l_line(const std::string& line, std::int64_t position) {
    std::string text = py_strip(line);
    if (text.compare(0, 3, "ERR") == 0) throw DcDeterminismError("worker reported: " + text);
    auto bad = [&]() {
        return DcDeterminismError("unexpected lockstep line for position " + std::to_string(position) + ": " +
                          py_repr(Json(head_code_points(text, 120))));
    };
    std::vector<std::string> fields = py_split(text);
    Lockstep row;
    std::int64_t index = 0;
    if (fields.size() != kLFields || fields[0] != "L" || !py_parse_int(fields[1], index) || index != position ||
        (fields[2] != "stopped" && fields[2] != "truncated") || (fields[4] != "stopped" && fields[4] != "truncated")) {
        throw bad();
    }
    double distance = 0.0;
    std::string reason;
    if (!py_float(Json(fields[10]), distance, reason) || !std::isfinite(distance) ||
        (distance < 0 && distance != -1)) {
        throw bad();
    }
    std::string payload;
    if (!base64_decode_strict(fields[15], payload) || payload.size() != 2 * kBoardSize) throw bad();
    row.status_a = fields[2];
    row.status_b = fields[4];
    if (!int_at_least(fields[3], 0, row.steps_a) || !int_at_least(fields[5], 0, row.steps_b) ||
        !int_at_least(fields[6], -1, row.first_bit_diff_step) || !int_at_least(fields[7], -1, row.first_gt[0]) ||
        !int_at_least(fields[8], -1, row.first_gt[1]) || !int_at_least(fields[9], -1, row.first_gt[2]) ||
        !int_at_least(fields[11], -1, row.max_step) || !int_at_least(fields[12], -1, row.max_stone) ||
        !int_at_least(fields[13], 0, row.presence_mismatch_pairs) ||
        !int_at_least(fields[14], -1, row.first_presence_mismatch_step)) {
        throw bad();
    }
    row.max_distance = distance;
    row.board_a = decode_board(payload, 0);  // DcDeterminismError for a non-finite coordinate
    row.board_b = decode_board(payload, kBoardSize);
    return row;
}

Json shot_row(const Json& shot, const Lockstep& parsed) {
    Json row = Json::object();
    row["status_a"] = parsed.status_a;
    row["steps_a"] = parsed.steps_a;
    row["status_b"] = parsed.status_b;
    row["steps_b"] = parsed.steps_b;
    row["first_bit_diff_step"] = parsed.first_bit_diff_step;
    for (std::size_t i = 0; i < 3; ++i) row[std::string("first_gt_") + kTrajectoryThresholds[i]] = parsed.first_gt[i];
    row["max_distance"] = parsed.max_distance;
    row["max_step"] = parsed.max_step;
    row["max_stone"] = parsed.max_stone;
    row["presence_mismatch_pairs"] = parsed.presence_mismatch_pairs;
    row["first_presence_mismatch_step"] = parsed.first_presence_mismatch_step;
    row["index"] = shot["index"];
    row["shot_id"] = shot["shot_id"];
    row["fgz"] = shot["fgz"];
    row["truncated"] = parsed.status_a == "truncated" || parsed.status_b == "truncated";
    row["step_count_difference"] = parsed.steps_b - parsed.steps_a;
    row["final"] = compare_boards(parsed.board_a, parsed.board_b);
    row["outcome"] = outcome_pair(parsed.board_a, parsed.board_b);
    return row;
}

Json log_summary_of(const std::vector<const Json*>& rows, const char* engine) {
    std::vector<const Json*> entries;
    for (const Json* row : rows) {
        if (row->contains("log")) entries.push_back(&(*row)["log"][engine]);
    }
    Json out = Json::object();
    out["shots"] = static_cast<std::int64_t>(entries.size());
    for (const char* basis : {"trajectory_last", "finish"}) {
        std::int64_t compared = 0, passed = 0, no1_equal = 0, scores_equal = 0;
        std::vector<double> errors;
        for (const Json* entry : entries) {
            const Json& comparison = (*entry)[basis];
            if (comparison.is_null()) continue;
            ++compared;
            passed += comparison["passed"].get<bool>() ? 1 : 0;
            no1_equal += comparison["no1_equal"].get<bool>() ? 1 : 0;
            scores_equal += comparison["scores_equal"].get<bool>() ? 1 : 0;
            errors.push_back(comparison["max_axis_error"].get<double>());
        }
        Json block = Json::object();
        block["compared"] = compared;
        block["passed"] = passed;
        block["no1_equal"] = no1_equal;
        block["scores_equal"] = scores_equal;
        block["max_axis_error"] = distribution(errors);
        out[basis] = block;
    }
    std::vector<std::pair<std::string, std::int64_t>> statuses;
    for (const Json* entry : entries) {
        std::string status = (*entry)["finish_status"].get<std::string>();
        auto found = std::find_if(statuses.begin(), statuses.end(),
                                  [&](const std::pair<std::string, std::int64_t>& item) { return item.first == status; });
        if (found == statuses.end()) {
            statuses.emplace_back(status, 1);
        } else {
            ++found->second;
        }
    }
    Json counts = Json::object();
    for (const auto& item : statuses) counts[item.first] = item.second;
    out["finish_status"] = counts;
    return out;
}

Json summarize(const Json& rows, bool logs) {
    std::vector<const Json*> usable;
    for (const Json& row : rows) {
        if (!row["truncated"].get<bool>()) usable.push_back(&row);
    }
    Json trajectory = Json::object();
    std::int64_t bit_difference_shots = 0, presence_shots = 0, presence_pairs = 0;
    std::vector<std::int64_t> first_bit_steps;
    std::vector<double> distances;
    std::int64_t first_gt_shots[3] = {0, 0, 0};
    for (const Json* row : usable) {
        std::int64_t first = (*row)["first_bit_diff_step"].get<std::int64_t>();
        if (first >= 0) {
            ++bit_difference_shots;
            first_bit_steps.push_back(first);
        }
        for (std::size_t i = 0; i < 3; ++i) {
            if ((*row)[std::string("first_gt_") + kTrajectoryThresholds[i]].get<std::int64_t>() >= 0) ++first_gt_shots[i];
        }
        double distance = (*row)["max_distance"].get<double>();
        if (distance >= 0) distances.push_back(distance);
        std::int64_t pairs = (*row)["presence_mismatch_pairs"].get<std::int64_t>();
        presence_shots += pairs != 0 ? 1 : 0;
        presence_pairs += pairs;
    }
    trajectory["bit_difference_shots"] = bit_difference_shots;
    trajectory["first_bit_diff_step"] = distribution_ints(first_bit_steps);
    for (std::size_t i = 0; i < 3; ++i) {
        trajectory[std::string("first_gt_") + kTrajectoryThresholds[i] + "_shots"] = first_gt_shots[i];
    }
    Json max_distance = distribution(distances);
    max_distance["above"] = threshold_counts(distances);
    trajectory["max_distance_m"] = max_distance;
    trajectory["presence_mismatch_shots"] = presence_shots;
    trajectory["presence_mismatch_pairs"] = presence_pairs;

    std::int64_t totals[4] = {0, 0, 0, 0};
    std::int64_t shots_gt_0 = 0, shots_gt_1cm = 0, stones_gt_0 = 0, stones_gt_1cm = 0, presence_difference_shots = 0;
    std::vector<double> finals;
    for (const Json* row : usable) {
        const Json& final = (*row)["final"];
        for (std::size_t i = 0; i < 4; ++i) totals[i] += final["classes"][kPresenceClasses[i]].get<std::int64_t>();
        std::int64_t gt_0 = final["stones_gt_0"].get<std::int64_t>();
        std::int64_t gt_1cm = final["stones_gt_1cm"].get<std::int64_t>();
        shots_gt_0 += gt_0 != 0 ? 1 : 0;
        shots_gt_1cm += gt_1cm != 0 ? 1 : 0;
        stones_gt_0 += gt_0;
        stones_gt_1cm += gt_1cm;
        presence_difference_shots += !final["presence_mismatch_slots"].empty() ? 1 : 0;
        if (!final["max_distance"].is_null()) finals.push_back(final["max_distance"].get<double>());
    }
    Json classes = Json::object();
    for (std::size_t i = 0; i < 4; ++i) classes[kPresenceClasses[i]] = totals[i];
    Json final_block = Json::object();
    final_block["stone_classes"] = classes;
    final_block["shots_d_gt_0"] = shots_gt_0;
    final_block["shots_d_gt_1cm"] = shots_gt_1cm;
    final_block["stones_d_gt_0"] = stones_gt_0;
    final_block["stones_d_gt_1cm"] = stones_gt_1cm;
    final_block["presence_difference_shots"] = presence_difference_shots;
    Json final_distance = distribution(finals);
    final_distance["above"] = threshold_counts(finals);
    final_block["max_distance_m"] = final_distance;
    final_block["centimetre_m"] = kCentimetre;

    Json no1 = Json::array(), scores = Json::array(), truncated_ids = Json::array();
    std::int64_t tie_changed = 0, step_count_difference = 0;
    for (const Json* row : usable) {
        if (!(*row)["outcome"]["no1_equal"].get<bool>()) no1.push_back((*row)["shot_id"]);
        if (!(*row)["outcome"]["scores_equal"].get<bool>()) scores.push_back((*row)["shot_id"]);
        tie_changed += (*row)["outcome"]["tie_equal"].get<bool>() ? 0 : 1;
        step_count_difference += (*row)["step_count_difference"].get<std::int64_t>() != 0 ? 1 : 0;
    }
    for (const Json& row : rows) {
        if (row["truncated"].get<bool>()) truncated_ids.push_back(row["shot_id"]);
    }
    Json outcome = Json::object();
    outcome["no1_changed"] = static_cast<std::int64_t>(no1.size());
    outcome["scores_changed"] = static_cast<std::int64_t>(scores.size());
    outcome["tie_changed"] = tie_changed;
    outcome["no1_changed_shots"] = no1;
    outcome["scores_changed_shots"] = scores;

    Json out = Json::object();
    out["shots"] = static_cast<std::int64_t>(rows.size());
    out["truncated"] = static_cast<std::int64_t>(rows.size() - usable.size());
    out["truncated_shots"] = truncated_ids;
    out["analysed"] = static_cast<std::int64_t>(usable.size());
    out["step_count_difference"] = step_count_difference;
    out["trajectory"] = trajectory;
    out["final_board"] = final_block;
    out["outcome"] = outcome;
    if (logs) {
        Json block = Json::object();
        for (const char* engine : kEngines) block[engine] = log_summary_of(usable, engine);
        out["log_comparison"] = block;
    }
    return out;
}

// What the final-board log comparison needs of a shot log: the recorded trajectory.
const JsonKeepTree& trajectory_keep() {
    static const JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "trajectory"});
        return tree;
    }();
    return keep;
}

}  // namespace

int command_compare_engines(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    bool with_logs = given(args, "logs_dir");
    if (args.has("compare_logs_tolerance") && !with_logs) {
        throw DcDeterminismError("--compare-logs-tolerance requires --logs-dir");
    }
    double tolerance = args.has("compare_logs_tolerance") ? args.real("compare_logs_tolerance") : 0.01;
    PreparedCorpus prepared = load_prepared(path_from_utf8(args.text_or("prepared", "")));
    SimulatorTarget target = resolve_simulator(args.text_or("compiler", ""), args.text_or("worker_command", ""));
    const ResolvedLoader& loader = target.loader;
    const Json& document = prepared.document;
    const Json& shots = document["shots"];
    std::size_t total = shots.size();
    std::int64_t requested_workers = args.integer("workers");
    int workers = static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(requested_workers, static_cast<std::int64_t>(total))));
    std::int64_t max_steps = args.integer("max_steps");

    Json contract = Json::object();
    contract["engines"] = Json::array({kEngines[0], kEngines[1]});
    contract["observation"] = kObservation;
    contract["max_steps"] = max_steps;
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = document["corpus"]["input_sha256"];
    contract["loader_sha256"] = loader.info["sha256"];

    std::vector<std::string> command = target.prefix;
    for (const std::string& part :
         {std::string("--loader"), path_text(loader.path), std::string("--engines"),
          std::string(kEngines[0]) + "," + kEngines[1], std::string("--input"), path_text(prepared.bin_path),
          std::string("--max-steps"), std::to_string(max_steps)}) {
        command.push_back(part);
    }

    auto started = Clock::now();
    struct ProgressState {
        double last = 0.0;
        long long last_count = -1;
    } progress_state;
    auto progress = [&](std::size_t done) {
        double now = std::chrono::duration<double>(Clock::now() - started).count();
        if (static_cast<long long>(done) != progress_state.last_count &&
            (done >= total || now - progress_state.last >= kProgressIntervalSeconds)) {
            progress_state.last = now;
            progress_state.last_count = static_cast<long long>(done);
            log("lockstep shots " + std::to_string(done) + "/" + std::to_string(total));
        }
    };

    log("lockstep of " + std::to_string(total) + " shots with " + std::to_string(workers) + " workers");
    std::vector<Lockstep> parsed(total);
    RunHooks hooks;
    hooks.request = [](std::int64_t position) { return "L " + std::to_string(position); };
    hooks.parse = [&parsed](const std::string& line, std::int64_t position) {
        parsed[static_cast<std::size_t>(position)] = parse_l_line(line, position);
        return Outcome();
    };
    run_repetition(command, total, workers, args.real("timeout"), progress, hooks);

    Json rows = Json::array();
    for (std::size_t i = 0; i < total; ++i) rows.push_back(shot_row(shots[i], parsed[i]));

    if (with_logs) {
        std::unique_ptr<Catalogue> catalogue = rebuild_catalogue(path_from_utf8(args.text_or("logs_dir", "")), prepared);
        std::vector<std::string> errors(total);
        std::atomic<std::size_t> next{0}, done{0}, first_failed{total}, running{0};
        int thread_count = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(workers, total)));
        running = static_cast<std::size_t>(thread_count);
        std::vector<std::thread> threads;
        for (int t = 0; t < thread_count; ++t) {
            threads.emplace_back([&] {
                ShotSource source(*catalogue);
                for (;;) {
                    std::size_t i = next.fetch_add(1);
                    if (i >= total || i > first_failed.load()) break;
                    try {
                        if (!rows[i]["truncated"].get<bool>()) {
                            LoadedShot item = source.load(source.locate(rows[i]["index"].get<std::int64_t>()),
                                                          &trajectory_keep());
                            Trajectory recorded = trajectory_from_log(item.log);
                            Json entry = Json::object();
                            const Board* boards[2] = {&parsed[i].board_a, &parsed[i].board_b};
                            for (std::size_t engine = 0; engine < 2; ++engine) {
                                entry[kEngines[engine]] =
                                    final_comparisons(item.log, item.fgz, *boards[engine], recorded.frames, tolerance);
                            }
                            rows[i]["log"] = std::move(entry);
                        }
                    } catch (const std::exception& exc) {
                        errors[i] = exc.what();
                        std::size_t seen = first_failed.load();
                        while (i < seen && !first_failed.compare_exchange_weak(seen, i)) {
                        }
                    }
                    done.fetch_add(1);
                }
                running.fetch_sub(1);
            });
        }
        auto last_report = Clock::now();
        while (running.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::duration<double>(Clock::now() - last_report).count() >= kProgressIntervalSeconds) {
                log("log comparison " + std::to_string(done.load()) + "/" + std::to_string(total));
                last_report = Clock::now();
            }
        }
        for (std::thread& thread : threads) thread.join();
        if (first_failed.load() < total) throw DcDeterminismError(errors[first_failed.load()]);
        catalogue.reset();
    }

    Json worker_counts = Json::object();
    worker_counts["requested"] = requested_workers;
    worker_counts["effective"] = workers;
    Json engines = Json::object();
    engines["a"] = kEngines[0];
    engines["b"] = kEngines[1];
    Json log_block = nullptr;
    if (with_logs) {
        log_block = Json::object();
        log_block["tolerance"] = tolerance;
        log_block["bases"] = Json::array({"trajectory_last", "finish"});
    }

    Json report = Json::object();
    report["schema"] = kSchema;
    report["contract"] = contract;
    report["engines"] = engines;
    report["loader"] = loader.info;
    report["worker"] = target.worker;
    report["preparer"] = document["preparer"];
    report["environment"] = environment::collect();
    report["workers"] = worker_counts;
    report["thresholds_m"] = thresholds_json();
    report["log_comparison"] = log_block;
    report["wall_s"] = std::chrono::duration<double>(Clock::now() - started).count();
    report["shots"] = rows;
    report["summary"] = summarize(rows, with_logs);
    write_json_new(output, report);
    const Json& summary = report["summary"];
    log("compare engines: " + std::to_string(total) + " shots, truncated " +
        std::to_string(summary["truncated"].get<std::int64_t>()) + ", bit differences " +
        std::to_string(summary["trajectory"]["bit_difference_shots"].get<std::int64_t>()) + ", final d>0 " +
        std::to_string(summary["final_board"]["shots_d_gt_0"].get<std::int64_t>()) + "; wrote " + path_text(output));
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
