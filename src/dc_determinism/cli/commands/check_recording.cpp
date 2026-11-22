// `compare pybind --check-recording`: the v2 recording engines against the official pybind binaries (Linux amd64,
// python3 and numpy in the dc_determinism-cpu:local image).
//
// For every shot of a prepared corpus and every engine (fcv1-py27, fcv1-py36):
//
// 1. the official extension runs simulator() -> final board;
// 2. the v2 recording extension runs simulator_frames() -> frames (F, 16, 6) = x, y, angle, vx, vy, w;
// 3. the final board the recording binary returns and the last frame (x, y, presence of the 16 slots) are
//    compared with the official final board: bitwise (the official double equals the recorded float32 widened to
//    double, bit for bit) and numerically (distance per slot).
//
// The recording runs use --removal-sheet-width none (no FCV1Plus play-area removal) unless told otherwise.
// Inputs are built exactly as `compare pybind` builds them (legacy pybind input, fcv1-py36 angular velocity
// negated).
//
// Exit code: 0 all record finals equal the official finals (bitwise), 1 at least one differs, 2 error (bad input,
// binary or worker failure, shot that could not be evaluated).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>

#include "cli/commands/pybind.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/pymath.hpp"
#include "common/text.hpp"
#include "corpus/prepared.hpp"
#include "corpus/pytext.hpp"
#include "json/writer.hpp"
#include "pybind/pybind.hpp"
#include "sim/metrics.hpp"

namespace dc_determinism {
namespace cli {

namespace {

const char* const kSchema = "dc-recording-v2-check-v3";

using Clock = std::chrono::steady_clock;

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// Official final board [team0, team1] -> 16 (x, y) points, slot = team * 8 + index.
std::vector<Point> board_of_final(const Json& final_board) {
    bool shape = final_board.is_array() && final_board.size() == 2;
    for (std::size_t team = 0; shape && team < 2; ++team) shape = final_board[team].is_array() && final_board[team].size() == 8;
    if (!shape) throw DcDeterminismError("unexpected official final-board shape");
    std::vector<Point> board;
    for (const Json& team : final_board) {
        for (const Json& point : team) {
            if (!point.is_array() || point.size() < 2 || !point[0].is_number() || !point[1].is_number()) {
                throw DcDeterminismError("invalid final-board point");
            }
            board.push_back({point[0].get<double>(), point[1].get<double>()});
        }
    }
    return board;
}

double widened_float(double value) { return static_cast<double>(static_cast<float>(value)); }

// The official final board is the pre-shot board (delivered stone removed): what get_stones() returns when the
// five-rock rule (applied_rule 0, total_shot_number < 5) undid the shot.
bool rule_restored(const Json& task, const std::vector<Point>& official) {
    if (task["applied_rule"].get<std::int64_t>() != 0 || task["total_shot_number"].get<std::int64_t>() >= 5) return false;
    const Json& start = task["initial_positions"];
    if (start.size() != 32) return false;
    for (std::size_t slot = 0; slot < official.size(); ++slot) {
        if (double_bits(widened_float(start[2 * slot].get<double>())) != double_bits(official[slot].x) ||
            double_bits(widened_float(start[2 * slot + 1].get<double>())) != double_bits(official[slot].y)) {
            return false;
        }
    }
    return true;
}

bool point_absent(double x, double y) { return std::fabs(x) <= kDisabledLimit && std::fabs(y) <= kDisabledLimit; }

Json slots_json(const std::vector<std::int64_t>& slots) {
    Json out = Json::array();
    for (std::int64_t slot : slots) out.push_back(slot);
    return out;
}

// One shot that both binaries answered.
struct Checked {
    std::string shot_id;
    bool final_return_equal = false;
    std::vector<std::int64_t> final_return_differing_slots;
    bool last_frame_equal = false;
    bool explained_by_rule_restoration = false;
    bool last_frame_strict_equal = false;
    std::vector<std::int64_t> official_removed_slots, missing_in_record_slots, position_differs_slots;
    double max_distance = 0.0;
    Json differing_slots = Json::array();
    std::int64_t frame_count = 0;
    double max_speed = 0.0;
    double max_abs_w = 0.0;
    bool all_zero = false;
    bool equal = false;
};

// Reads a number of a worker row the way the Python `float()` of a JSON number does.
double number_at(const Json& value) {
    if (!value.is_number()) throw DcDeterminismError("not a number");
    return value.get<double>();
}

// The official final board against (a) the final board that simulator_frames() returns and (b) the last recorded
// frame.
//
// (a) must be identical, bit for bit: it is the same pipeline with a read-only recorder attached. (b) is compared
// slot by slot. The official engine zeroes (0, 0) the stones it removes from the play area at the end of the
// shot, while the recorder keeps their bodies; such slots are listed as `official_removed_slots` and are not
// differences. A difference is an official stone that is absent from the last frame or sits elsewhere
// (`missing_in_record_slots`, `position_differs_slots`).
//
// The recorder samples the step loop only. simulator() then runs get_stones(), which for the first shots of an
// end may restore the whole pre-shot board (five-rock rule). Such a shot has a last frame that differs from the
// official final by design; it is reported as `explained_by_rule_restoration` when the official final equals the
// pre-shot board bit for bit (float32), and does not count as a difference.
Checked check_shot(const Json& task, const Json& official_final, const Json& record_row) {
    Checked out;
    out.shot_id = task["shot_id"].get<std::string>();
    std::vector<Point> official = board_of_final(official_final);
    std::vector<Point> returned = board_of_final(record_row.at("final"));
    const Json& last_frame = record_row.at("last_frame");
    if (!last_frame.is_array() || last_frame.size() != 16) throw DcDeterminismError("unexpected last frame shape");
    for (std::size_t slot = 0; slot < 16; ++slot) {
        if (!last_frame[slot].is_array() || last_frame[slot].size() != 6) throw DcDeterminismError("unexpected last frame shape");
    }
    for (std::size_t slot = 0; slot < official.size(); ++slot) {
        if (double_bits(official[slot].x) != double_bits(returned[slot].x) ||
            double_bits(official[slot].y) != double_bits(returned[slot].y)) {
            out.final_return_differing_slots.push_back(static_cast<std::int64_t>(slot));
        }
    }
    out.final_return_equal = out.final_return_differing_slots.empty();
    bool strict = true;
    std::vector<double> distances;
    for (std::size_t slot = 0; slot < official.size(); ++slot) {
        double ox = official[slot].x, oy = official[slot].y;
        double rx = number_at(last_frame[slot][0]), ry = number_at(last_frame[slot][1]);
        bool equal = double_bits(ox) == double_bits(rx) && double_bits(oy) == double_bits(ry);
        strict = strict && equal;
        bool official_absent = point_absent(ox, oy), record_absent = point_absent(rx, ry);
        if (official_absent && !record_absent) {
            out.official_removed_slots.push_back(static_cast<std::int64_t>(slot));
        } else if (record_absent && !official_absent) {
            out.missing_in_record_slots.push_back(static_cast<std::int64_t>(slot));
        } else if (!official_absent && !equal) {
            out.position_differs_slots.push_back(static_cast<std::int64_t>(slot));
        }
        if (!official_absent && !record_absent) distances.push_back(py_hypot(ox - rx, oy - ry));
        if (!equal) {
            Json detail = Json::object();
            detail["slot"] = static_cast<std::int64_t>(slot);
            detail["official"] = Json::array({ox, oy});
            detail["record_v2_last_frame"] = Json::array({rx, ry});
            out.differing_slots.push_back(std::move(detail));
        }
    }
    out.last_frame_equal = out.missing_in_record_slots.empty() && out.position_differs_slots.empty();
    out.explained_by_rule_restoration = !out.last_frame_equal && rule_restored(task, official);
    out.last_frame_strict_equal = strict;
    out.max_distance = distances.empty() ? 0.0 : *std::max_element(distances.begin(), distances.end());

    // end velocity of the last frame
    double max_speed = 0.0, max_abs_w = 0.0;
    bool all_zero = true;
    for (std::size_t slot = 0; slot < 16; ++slot) {
        double vx = number_at(last_frame[slot][3]), vy = number_at(last_frame[slot][4]), w = number_at(last_frame[slot][5]);
        if (slot == 0 || py_hypot(vx, vy) > max_speed) max_speed = py_hypot(vx, vy);
        if (slot == 0 || std::fabs(w) > max_abs_w) max_abs_w = std::fabs(w);
        all_zero = all_zero && vx == 0 && vy == 0 && w == 0;
    }
    out.max_speed = max_speed;
    out.max_abs_w = max_abs_w;
    out.all_zero = all_zero;

    std::int64_t frames = 0;
    std::string reason;
    if (!py_int(record_row.at("frame_count"), frames, reason)) throw DcDeterminismError("invalid frame_count");
    out.frame_count = frames;
    std::int64_t steps = 0;
    if (!py_int(record_row.at("steps"), steps, reason)) throw DcDeterminismError("invalid steps");
    out.equal = out.final_return_equal && (out.last_frame_equal || out.explained_by_rule_restoration);
    return out;
}

// {count, min, max, mean} of integers (Python ints: the sum is exact, the mean a true division).
Json int_distribution(const std::vector<std::int64_t>& values) {
    Json out = Json::object();
    out["count"] = static_cast<std::int64_t>(values.size());
    if (values.empty()) return out;
    std::vector<std::int64_t> ordered = values;
    std::sort(ordered.begin(), ordered.end());
    std::int64_t sum = 0;
    for (std::int64_t value : ordered) sum += value;
    out["min"] = ordered.front();
    out["max"] = ordered.back();
    out["mean"] = static_cast<double>(sum) / static_cast<double>(ordered.size());
    return out;
}

struct EngineCheck {
    Json report;
    std::int64_t errors = 0;
    std::int64_t differing = 0;
};

struct EngineInputs {
    std::string name;  // py27 / py36
    PybindBinary official, record;
};

EngineCheck check_engine(const PybindWorker& worker, const EngineInputs& binaries, const std::vector<Json>& tasks,
                         const std::vector<double>& widths, std::int64_t frame_stride, double timeout, int workers) {
    std::string engine = pybind_name(binaries.name);
    std::vector<std::pair<std::int64_t, Json>> runnable;
    for (std::size_t position = 0; position < tasks.size(); ++position) {
        if (tasks[position].is_null()) continue;
        Json task = adapt_task(tasks[position], binaries.name);
        task["removal_sheet_width"] = widths[position];
        runnable.emplace_back(static_cast<std::int64_t>(position), std::move(task));
    }
    log(engine + ": " + std::to_string(runnable.size()) + " shots, official + record-v2");
    std::map<std::int64_t, Json> official = run_rows(worker, binaries.name, binaries.official, "official", runnable, 1,
                                                     timeout, workers, binaries.name + " official");
    std::map<std::int64_t, Json> record = run_rows(worker, binaries.name, binaries.record, "record", runnable,
                                                   frame_stride, timeout, workers, binaries.name + " record-v2");
    struct Row {
        std::string shot_id;
        Json error;  // null: none
        Checked checked;
    };
    std::vector<Row> rows;
    std::int64_t equal = 0, errors = 0;
    for (const auto& item : runnable) {
        std::int64_t position = item.first;
        const Json& task = tasks[static_cast<std::size_t>(position)];
        Row row;
        row.shot_id = task["shot_id"].get<std::string>();
        const Json& a = official[position];
        const Json& b = record[position];
        if (a.contains("error") || b.contains("error")) {
            Json error = Json::object();
            error["official"] = a.contains("error") ? a["error"] : Json(nullptr);
            error["record_v2"] = b.contains("error") ? b["error"] : Json(nullptr);
            row.error = error;
            ++errors;
        } else {
            try {
                row.checked = check_shot(task, a.at("final"), b);
                equal += row.checked.equal ? 1 : 0;
            } catch (const std::exception& exc) {
                row.error = std::string("invalid result: ") + exc.what();
                ++errors;
            }
        }
        rows.push_back(std::move(row));
    }

    std::vector<const Row*> compared, differing;
    for (const Row& row : rows) {
        if (!row.error.is_null()) continue;
        compared.push_back(&row);
        if (!row.checked.equal) differing.push_back(&row);
    }
    std::int64_t final_return_equal = 0, last_frame_equal = 0, explained = 0, strict_equal = 0, removed_shots = 0,
                 removed_stones = 0, all_zero = 0;
    double max_distance = 0.0, max_speed = 0.0, max_abs_w = 0.0;
    std::vector<std::int64_t> frame_counts;
    for (const Row* row : compared) {
        const Checked& c = row->checked;
        final_return_equal += c.final_return_equal ? 1 : 0;
        last_frame_equal += c.last_frame_equal ? 1 : 0;
        explained += c.explained_by_rule_restoration ? 1 : 0;
        strict_equal += c.last_frame_strict_equal ? 1 : 0;
        removed_shots += c.official_removed_slots.empty() ? 0 : 1;
        removed_stones += static_cast<std::int64_t>(c.official_removed_slots.size());
        all_zero += c.all_zero ? 1 : 0;
        if (!c.explained_by_rule_restoration && c.max_distance > max_distance) max_distance = c.max_distance;
        if (c.max_speed > max_speed) max_speed = c.max_speed;
        if (c.max_abs_w > max_abs_w) max_abs_w = c.max_abs_w;
        frame_counts.push_back(c.frame_count);
    }
    Json differing_shots = Json::array();
    for (const Row* row : differing) {
        const Checked& c = row->checked;
        Json item = Json::object();
        item["shot_id"] = c.shot_id;
        item["final_return_differing_slots"] = slots_json(c.final_return_differing_slots);
        item["missing_in_record_slots"] = slots_json(c.missing_in_record_slots);
        item["position_differs_slots"] = slots_json(c.position_differs_slots);
        item["max_distance"] = c.max_distance;
        item["differing_slots"] = c.differing_slots;
        differing_shots.push_back(std::move(item));
    }
    Json error_shots = Json::array();
    for (const Row& row : rows) {
        if (row.error.is_null()) continue;
        Json item = Json::object();
        item["shot_id"] = row.shot_id;
        item["error"] = row.error;
        error_shots.push_back(std::move(item));
    }
    Json velocity = Json::object();
    velocity["shots_all_zero"] = all_zero;
    velocity["shots_not_zero"] = static_cast<std::int64_t>(compared.size()) - all_zero;
    velocity["max_speed"] = max_speed;
    velocity["max_abs_w"] = max_abs_w;

    EngineCheck out;
    Json report = Json::object();
    report["engine"] = engine;
    report["shots"] = static_cast<std::int64_t>(runnable.size());
    report["compared"] = static_cast<std::int64_t>(compared.size());
    report["equal"] = equal;
    report["differing"] = static_cast<std::int64_t>(differing.size());
    report["errors"] = errors;
    report["max_distance_m"] = max_distance;
    report["final_return_equal"] = final_return_equal;
    report["last_frame_equal"] = last_frame_equal;
    report["explained_by_rule_restoration"] = explained;
    report["last_frame_strict_equal"] = strict_equal;
    report["shots_with_official_removed_stones"] = removed_shots;
    report["official_removed_stones"] = removed_stones;
    report["differing_shots"] = differing_shots;
    report["error_shots"] = error_shots;
    report["frame_count"] = int_distribution(frame_counts);
    report["end_velocity"] = velocity;
    out.report = std::move(report);
    out.errors = errors;
    out.differing = static_cast<std::int64_t>(differing.size());
    return out;
}

}  // namespace

int command_check_recording(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    std::int64_t frame_stride = args.has("frame_stride") ? args.integer("frame_stride") : 1;
    std::int64_t limit = args.has("limit") ? args.integer("limit") : 0;
    if (frame_stride < 1 || limit < 0) throw DcDeterminismError("--frame-stride must be >= 1 and --limit >= 0");
    std::vector<std::string> names;
    {
        std::string text = args.text_or("engines", "py27,py36");
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t comma = text.find(',', start);
            std::string part = py_strip(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!part.empty()) names.push_back(part);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    bool valid = !names.empty();
    for (const std::string& name : names) {
        valid = valid && std::find(pybind_engines().begin(), pybind_engines().end(), name) != pybind_engines().end();
    }
    if (!valid) throw DcDeterminismError("--engines takes py27 and/or py36");
    std::vector<std::string> unique;
    for (const std::string& name : names) {
        if (std::find(unique.begin(), unique.end(), name) == unique.end()) unique.push_back(name);
    }
    names = unique;

    PybindWorker worker(args.text_or("pybind_worker_command", ""));
    std::vector<EngineInputs> binaries;
    for (const std::string& name : names) {
        EngineInputs inputs;
        inputs.name = name;
        inputs.official = resolve_pybind_binary(name, "official", args.text_or("official_" + name, ""));
        inputs.record = resolve_pybind_binary(name, "record", args.text_or("record_" + name, ""));
        binaries.push_back(std::move(inputs));
    }

    PreparedCorpus prepared = load_prepared(path_from_utf8(args.text_or("prepared", "")));
    const Json& document = prepared.document;
    std::size_t count = document["shots"].size();
    if (limit != 0) count = std::min<std::size_t>(count, static_cast<std::size_t>(limit));
    std::string width_text = args.text_or("removal_sheet_width", "none");
    std::vector<double> widths;
    if (width_text == "shot") {
        widths = read_widths(prepared, count);
    } else if (width_text == "none") {
        widths.assign(count, 0.0);
    } else {
        double metres = 0.0;
        std::string reason;
        if (!py_float(Json(width_text), metres, reason) || !std::isfinite(metres)) {
            throw DcDeterminismError("--removal-sheet-width takes none, shot or a number of metres, not " + width_text);
        }
        widths.assign(count, metres);
    }
    auto started = Clock::now();
    std::int64_t requested_workers = args.integer("workers");
    int workers = static_cast<int>(std::max<std::int64_t>(1, requested_workers));
    double timeout = args.real("timeout");

    std::vector<Json> tasks(count);
    std::vector<std::pair<std::string, std::string>> input_errors;
    {
        std::unique_ptr<Catalogue> catalogue = rebuild_catalogue(path_from_utf8(args.text_or("logs_dir", "")), prepared);
        ShotSource source(*catalogue);
        static const JsonKeepTree keep = [] {
            JsonKeepTree tree;
            tree.add({"log", "end"});
            tree.add({"log", "shot"});
            tree.add({"log", "actual_move"});
            tree.add({"log", "simulator_storage"});
            tree.add({"log", "trajectory", "frames"});
            return tree;
        }();
        for (std::size_t position = 0; position < count; ++position) {
            const Json& shot = document["shots"][position];
            try {
                LoadedShot item = source.load(source.locate(shot["index"].get<std::int64_t>()), &keep);
                Json task = normalize_shot(item);
                if (task["shot_id"] != shot["shot_id"]) {
                    throw DcDeterminismError("ValueError: shot id " + py_repr(task["shot_id"]) + " differs from " +
                                     py_repr(shot["shot_id"]));
                }
                tasks[position] = std::move(task);
            } catch (const DcDeterminismError& exc) {
                input_errors.emplace_back(shot["shot_id"].get<std::string>(), exc.what());
            }
        }
        source.close();
    }

    Json engine_reports = Json::object();
    std::int64_t errors = 0, differing = 0;
    for (const EngineInputs& inputs : binaries) {
        EngineCheck check = check_engine(worker, inputs, tasks, widths, frame_stride, timeout, workers);
        engine_reports[inputs.name] = check.report;
        errors += check.errors;
        differing += check.differing;
    }

    Json binary_block = Json::object();
    for (const EngineInputs& inputs : binaries) {
        Json entry = Json::object();
        entry["official_sha256"] = inputs.official.sha256;
        entry["record_v2_sha256"] = inputs.record.sha256;
        binary_block[inputs.name] = entry;
    }
    Json contract = Json::object();
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = document["corpus"]["input_sha256"];
    contract["shots"] = static_cast<std::int64_t>(count);
    contract["frame_stride"] = frame_stride;
    contract["removal_sheet_width"] = width_text;
    contract["frame_layout"] = "float32 (F, 16, 6) = x, y, angle, vx, vy, w";
    contract["equal_means"] =
        "the final board returned by simulator_frames() equals the official final board bit for bit, and every "
        "stone of the official final board equals the last recorded frame bit for bit (x, y widened float32 -> "
        "double); stones the official engine removed at the end of the shot (official (0, 0)) are still present "
        "in the last frame and are reported separately; a shot whose last frame differs because get_stones() "
        "restored the pre-shot board (five-rock rule) counts as equal when the official final is exactly that board";
    contract["binaries"] = binary_block;
    contract["manifest_build"] = pybind_manifest().contains("build") ? pybind_manifest()["build"] : Json(nullptr);

    Json provenance = Json::object();
    provenance["pybind_worker"] = worker.info();
    provenance["python_version"] = worker.python_version();
    provenance["numpy_version"] = worker.numpy_version();
    provenance["wall_s"] = std::chrono::duration<double>(Clock::now() - started).count();

    Json input_error_list = Json::array();
    for (const auto& item : input_errors) {
        Json entry = Json::object();
        entry["shot_id"] = item.first;
        entry["error"] = item.second;
        input_error_list.push_back(std::move(entry));
    }
    Json report = Json::object();
    report["schema"] = kSchema;
    report["contract"] = contract;
    report["provenance"] = provenance;
    report["engines"] = engine_reports;
    report["input_errors"] = input_error_list;
    report["all_finals_equal"] = differing == 0 && errors == 0;
    report["errors"] = errors;
    write_json_new(output, report);
    for (auto it = engine_reports.begin(); it != engine_reports.end(); ++it) {
        const Json& r = it.value();
        log(it.key() + ": equal " + std::to_string(r["equal"].get<std::int64_t>()) + "/" +
            std::to_string(r["shots"].get<std::int64_t>()) + ", differing " +
            std::to_string(r["differing"].get<std::int64_t>()) + ", errors " +
            std::to_string(r["errors"].get<std::int64_t>()) + ", max distance " + format_g(r["max_distance_m"].get<double>()));
    }
    log("wrote " + path_text(output));
    if (errors != 0 || !input_errors.empty()) return 2;
    return differing != 0 ? 1 : 0;
}

}  // namespace cli
}  // namespace dc_determinism
