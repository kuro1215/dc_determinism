// `compare pybind`: Path A (FCV1 through the C API) against Path B (the historical official pybind extensions
// fcv1-py27 and fcv1-py36), Linux amd64 only.
//
// Both official binaries run on every shot. Path A final boards come from our worker (`O <pos> -`), Path B final
// boards from `src/worker/pybind/pybind_worker.py` with the legacy pybind input of each old log
// (pybind/pybind.hpp). The pybind final board `[team0, team1]` maps onto the C API slots `team * 8 + index`; a
// pybind stone is absent when both coordinates are within 1e-7 of zero, a C API stone when its mask bit is
// clear. Optionally the v2 recording binaries (`prebuilt/pybind/<engine>/simulator-record-v2.so`, checked
// against `prebuilt/pybind/manifest.json`) replay listed shots frame by frame; the Path A capture and the
// recording are compared with the metrics of `compare simulator` on capture archives (all six fields of every
// stone), and the final board the recording binary returns is compared bit for bit with the official final.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>

#include "capture/collector.hpp"
#include "capture/compare_traj.hpp"
#include "capture/dctr.hpp"
#include "cli/commands/capture.hpp"
#include "cli/commands/pybind.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/tempdir.hpp"
#include "common/text.hpp"
#include "corpus/prepared.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"
#include "logs/log_compare.hpp"
#include "pybind/pybind.hpp"
#include "sim/metrics.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {

namespace {

const char* const kSchema = "dc-determinism-pybind-deviation-v4";
const char* const kObservation = "raw-stonecoordinate-v1";
const char* const kPresenceClasses[4] = {"both_present", "both_absent", "a_only", "b_only"};
constexpr double kProgressIntervalSeconds = 5.0;

using Clock = std::chrono::steady_clock;

const char* const kKnownDifferences[] = {
    "the pybind extensions return one final board per shot with 8 slots per team; the C API worker steps with the "
    "shot's sheet width, the official pybind call takes none",
    "pybind final-board stones are absent when both coordinates are within 1e-7 of zero",
};

// `O <pos> -`: the result line plus the final board; nothing is sampled.
class FinalsObserver : public RunObserver {
public:
    explicit FinalsObserver(std::size_t count) : boards(count) {}

    std::string request(std::int64_t position) override { return "O " + std::to_string(position) + " -"; }

    void consume(std::int64_t position, const Outcome&, const std::string& line) override {
        std::pair<std::int64_t, std::string> decoded = decode_s_line(line, position);
        if (decoded.first != 0) {
            throw DcDeterminismError("position " + std::to_string(position) + ": worker returned " +
                             std::to_string(decoded.first) + " sampled boards for a final-only request");
        }
        boards[static_cast<std::size_t>(position)] = decode_payload(decoded.second, 0).first;
    }

    void finish(double) override {}
    void cancel() override {}

    std::vector<Board> boards;
};

bool given(const cli::ParsedArgs& args, const char* dest) {
    return args.has(dest) && !args.text_or(dest, "").empty();
}

// What the legacy input of a shot needs of its log.
const JsonKeepTree& input_keep() {
    static const JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "end"});
        tree.add({"log", "shot"});
        tree.add({"log", "actual_move"});
        tree.add({"log", "simulator_storage"});
        tree.add({"log", "trajectory", "frames"});
        return tree;
    }();
    return keep;
}

// One shot, one engine: Path A board against a pybind final board.
Json compare_shot(const Board& board_a, const Json& final_b) {
    Board board_b;
    try {
        board_b = pybind_board(final_b);
    } catch (const DcDeterminismError& exc) {
        Json out = Json::object();
        out["status"] = "inconclusive";
        out["reason"] = std::string("invalid final board: ") + exc.what();
        return out;
    }
    Json out = Json::object();
    out["status"] = "compared";
    Json boards = compare_boards(board_a, board_b);
    for (auto it = boards.begin(); it != boards.end(); ++it) out[it.key()] = it.value();
    out["outcome"] = outcome_pair(board_a, board_b);
    return out;
}

// `grouped`: `all` plus one summary per tournament for the entries `pick` selects.
Json grouped(const Json& rows, const std::string& engine) {
    std::vector<const Json*> all;
    std::map<std::string, std::vector<const Json*>> by_tournament;
    for (const Json& row : rows) {
        const Json& entry = row[engine];
        all.push_back(&entry);
        by_tournament[row["tournament"].get<std::string>()].push_back(&entry);
    }
    Json out = Json::object();
    out["all"] = pybind_group_summary(all);
    Json tournaments = Json::object();
    for (const auto& item : by_tournament) tournaments[item.first] = pybind_group_summary(item.second);
    out["by_tournament"] = tournaments;
    return out;
}

struct TrajectoryContext {
    const PybindWorker* worker = nullptr;
    std::map<std::string, PybindBinary> records;
    const std::vector<std::int64_t>* positions = nullptr;
    const std::vector<Json>* tasks = nullptr;  // null: no pybind input
    const Json* shots = nullptr;
    const std::vector<Outcome>* path_a = nullptr;
    const std::vector<std::string>* prefix = nullptr;
    const ResolvedLoader* loader = nullptr;
    const PreparedCorpus* prepared = nullptr;
    const std::map<std::pair<std::string, std::int64_t>, Json>* raw_finals = nullptr;
    int workers = 1;
    std::int64_t max_steps = 0;
    double timeout = 0.0;
};

// Path A captures of the listed shots against the v2 recording binaries' frames, compared with the metrics of
// `compare simulator` on capture archives.
Json trajectory_section(const TrajectoryContext& ctx) {
    const Json& shots = *ctx.shots;
    std::map<std::int64_t, std::shared_ptr<CapturedTrajectory>> captured;
    std::mutex captured_mutex;

    std::vector<std::int64_t> usable;
    for (std::int64_t position : *ctx.positions) {
        std::size_t at = static_cast<std::size_t>(position);
        if (!(*ctx.tasks)[at].is_null() && (*ctx.path_a)[at].status == "stopped") usable.push_back(position);
    }
    if (!usable.empty()) {
        TempDir capture_dir("dc_determinism-pybind-capture-");
        std::vector<std::string> command = *ctx.prefix;
        for (const std::string& part :
             {std::string("--loader"), path_text(ctx.loader->path), std::string("--engine"), std::string("fcv1"),
              std::string("--input"), path_text(ctx.prepared->bin_path), std::string("--max-steps"),
              std::to_string(ctx.max_steps), std::string("--capture-dir"), path_text(capture_dir.path())}) {
            command.push_back(part);
        }
        auto sink = [&](std::int64_t position, const Outcome&, std::string&& payload, std::int64_t) {
            std::shared_ptr<CapturedTrajectory> trajectory = std::make_shared<CapturedTrajectory>(std::move(payload));
            std::lock_guard<std::mutex> lock(captured_mutex);
            captured[position] = std::move(trajectory);
        };
        TrajectoryCollector collector(capture_dir.path(), "fcv1", shots, sink);
        log("path A trajectories of " + std::to_string(usable.size()) + " shots");
        RunHooks hooks;
        hooks.observer = &collector;
        hooks.positions = &usable;
        int workers = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(ctx.workers, usable.size())));
        run_repetition(command, usable.size(), workers, 0.0, nullptr, hooks);
    }

    std::vector<double> widths = read_widths(*ctx.prepared, shots.size());
    std::map<std::pair<std::string, std::int64_t>, std::pair<std::shared_ptr<CapturedTrajectory>, Json>> recorded;
    std::map<std::string, std::string> failures;
    for (const std::string& engine : pybind_engines()) {
        auto binary = ctx.records.find(engine);
        if (binary == ctx.records.end()) continue;
        std::vector<std::pair<std::int64_t, Json>> items;
        for (std::int64_t position : *ctx.positions) {
            if (captured.count(position) == 0) continue;
            Json task = adapt_task((*ctx.tasks)[static_cast<std::size_t>(position)], engine);
            task["removal_sheet_width"] = widths[static_cast<std::size_t>(position)];
            items.emplace_back(position, std::move(task));
        }
        if (items.empty()) continue;
        std::mutex recorded_mutex;
        auto consume = [&](std::int64_t position, const Json& row, std::vector<float>&& frames) {
            const Json& shot = shots[static_cast<std::size_t>(position)];
            std::int64_t frame_count = static_cast<std::int64_t>(frames.size() / (kFrameSlots * kFrameFields));
            std::string payload = frames_to_payload(frames, frame_count, shot["shot_id"].get<std::string>(),
                                                    shot["index"].get<std::int64_t>(), "pybind-" + engine);
            std::shared_ptr<CapturedTrajectory> trajectory = std::make_shared<CapturedTrajectory>(std::move(payload));
            std::lock_guard<std::mutex> lock(recorded_mutex);
            recorded[{engine, position}] = {std::move(trajectory), row};
        };
        log(pybind_name(engine) + " recording of " + std::to_string(items.size()) + " shots");
        int workers = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(ctx.workers, items.size())));
        try {
            record_trajectories(*ctx.worker, engine, binary->second, items, ctx.timeout, workers, consume);
        } catch (const DcDeterminismError& failure) {
            failures[engine] = failure.what();
        }
    }

    Json out = Json::array();
    for (std::int64_t position : *ctx.positions) {
        std::size_t at = static_cast<std::size_t>(position);
        Json base = Json::object();
        base["shot_id"] = shots[at]["shot_id"];
        for (const std::string& engine : pybind_engines()) {
            if (ctx.records.count(engine) == 0) continue;
            Json entry = base;
            entry["engine"] = engine;
            auto inconclusive = [&](const std::string& reason) {
                entry["status"] = "inconclusive";
                entry["reason"] = reason;
                out.push_back(entry);
            };
            if (captured.count(position) == 0) {
                inconclusive("no path A capture");
                continue;
            }
            auto found = recorded.find({engine, position});
            if (found == recorded.end()) {
                auto failure = failures.find(engine);
                inconclusive(failure == failures.end() ? std::string("no recording") : failure->second);
                continue;
            }
            const Json& row = found->second.second;
            Json comparison;
            std::int64_t steps_b = 0;
            try {
                comparison = compare_trajectories(*captured[position], *found->second.first);
                std::string reason;
                if (!row.contains("steps") || !py_int(row["steps"], steps_b, reason)) {
                    throw DcDeterminismError(reason.empty() ? std::string("missing 'steps'") : reason);
                }
            } catch (const DcDeterminismError& exc) {
                inconclusive(std::string("invalid recording row: ") + exc.what());
                continue;
            }
            std::int64_t steps_a = (*ctx.path_a)[at].steps;
            entry["status"] = "compared";
            for (auto it = comparison.begin(); it != comparison.end(); ++it) entry[it.key()] = it.value();
            entry["steps_a"] = steps_a;
            entry["steps_b"] = steps_b;
            entry["step_count_difference"] = steps_b - steps_a;
            entry["removed_steps"] = static_cast<std::int64_t>(
                row.contains("removed_steps") && row["removed_steps"].is_array() ? row["removed_steps"].size() : 0);
            auto official = ctx.raw_finals->find({engine, position});
            if (official == ctx.raw_finals->end()) {
                entry["recording_official_final_equal"] = nullptr;
            } else {
                entry["recording_official_final_equal"] =
                    bitwise_final_equal(row.contains("final") ? row["final"] : Json(nullptr), official->second);
            }
            out.push_back(entry);
        }
    }
    return out;
}

}  // namespace

std::string tournament_of(const std::string& container, const std::string& logs_dir) {
    if (container.empty()) return "unknown";
    fs::path path = path_from_utf8(container);
    std::vector<std::string> parts;
    bool relative = false;
    if (!logs_dir.empty()) {
        fs::path root = path_from_utf8(logs_dir);
        auto root_it = root.begin();
        auto path_it = path.begin();
        // Path.relative_to: the path must start with every component of the directory.
        std::vector<std::string> rest;
        bool prefix = true;
        for (; root_it != root.end(); ++root_it, ++path_it) {
            if (path_it == path.end() || *path_it != *root_it) {
                prefix = false;
                break;
            }
        }
        if (prefix) {
            for (; path_it != path.end(); ++path_it) rest.push_back(path_to_utf8(*path_it));
            parts = rest;
            relative = true;
        }
    }
    if (!relative) {
        if (logs_dir.empty()) {
            for (const fs::path& part : path) {
                std::string text = path_to_utf8(part);
                if (text != "/" && text != "\\") parts.push_back(text);
            }
        } else {
            parts.push_back(path_to_utf8(path.filename()));
        }
    }
    for (const std::string& part : parts) {
        std::string lower = ascii_lower(part);
        std::size_t i = 0;
        while (i < lower.size() && lower[i] >= 'a' && lower[i] <= 'z') ++i;
        std::size_t letters = i;
        while (i < lower.size() && lower[i] >= '0' && lower[i] <= '9') ++i;
        if (letters > 0 && i > letters) return lower.substr(0, i);
    }
    return "unknown";
}

Board pybind_board(const Json& final_board) {
    if (!final_board.is_array() || final_board.size() != 2) throw DcDeterminismError("incompatible final-board shape");
    for (const Json& team : final_board) {
        if (!team.is_array() || team.size() > 8) throw DcDeterminismError("incompatible final-board shape");
    }
    Board board;
    for (std::size_t team = 0; team < 2; ++team) {
        for (std::size_t index = 0; index < final_board[team].size(); ++index) {
            const Json& point = final_board[team][index];
            if (!point.is_array() || point.size() < 2 || !point[0].is_number() || !point[1].is_number()) {
                throw DcDeterminismError("invalid final-board point");
            }
            double x = point[0].get<double>();
            double y = point[1].get<double>();
            if (!(std::isfinite(x) && std::isfinite(y))) throw DcDeterminismError("non-finite final-board point");
            Stone& stone = board[team * 8 + index];
            bool absent = std::fabs(x) <= kDisabledLimit && std::fabs(y) <= kDisabledLimit;
            stone.present = !absent;
            stone.x = absent ? 0.0 : x;
            stone.y = absent ? 0.0 : y;
        }
    }
    return board;
}

Json pybind_group_summary(const std::vector<const Json*>& entries) {
    std::vector<const Json*> done;
    for (const Json* entry : entries) {
        if ((*entry)["status"] == "compared") done.push_back(entry);
    }
    Json reasons = Json::object();
    for (const Json* entry : entries) {
        if ((*entry)["status"] != "compared") {
            const std::string& reason = (*entry)["reason"].get_ref<const std::string&>();
            reasons[reason] = reasons.contains(reason) ? reasons[reason].get<std::int64_t>() + 1 : 1;
        }
    }
    std::int64_t classes[4] = {0, 0, 0, 0};
    for (const Json* entry : done) {
        for (std::size_t i = 0; i < 4; ++i) classes[i] += (*entry)["classes"][kPresenceClasses[i]].get<std::int64_t>();
    }
    std::vector<double> shot_max, stone_d;
    std::int64_t shots_gt_0 = 0, shots_gt_1cm = 0, shots_presence = 0, stones_gt_0 = 0, stones_gt_1cm = 0;
    std::int64_t no1_changed = 0, scores_changed = 0;
    for (const Json* entry : done) {
        const Json& e = *entry;
        if (!e["max_distance"].is_null()) shot_max.push_back(e["max_distance"].get<double>());
        for (auto it = e["distances"].begin(); it != e["distances"].end(); ++it) stone_d.push_back(it.value().get<double>());
        shots_gt_0 += e["stones_gt_0"].get<std::int64_t>() != 0 ? 1 : 0;
        shots_gt_1cm += e["stones_gt_1cm"].get<std::int64_t>() != 0 ? 1 : 0;
        shots_presence += !e["presence_mismatch_slots"].empty() ? 1 : 0;
        stones_gt_0 += e["stones_gt_0"].get<std::int64_t>();
        stones_gt_1cm += e["stones_gt_1cm"].get<std::int64_t>();
        no1_changed += e["outcome"]["no1_equal"].get<bool>() ? 0 : 1;
        scores_changed += e["outcome"]["scores_equal"].get<bool>() ? 0 : 1;
    }
    std::int64_t conclusive = static_cast<std::int64_t>(done.size());
    Json class_block = Json::object();
    for (std::size_t i = 0; i < 4; ++i) class_block[kPresenceClasses[i]] = classes[i];
    Json shot_distribution = distribution(shot_max);
    shot_distribution["above"] = threshold_counts(shot_max);
    Json outcome = Json::object();
    outcome["no1_equal"] = conclusive - no1_changed;
    outcome["no1_changed"] = no1_changed;
    outcome["scores_equal"] = conclusive - scores_changed;
    outcome["scores_changed"] = scores_changed;

    Json out = Json::object();
    out["shots"] = static_cast<std::int64_t>(entries.size());
    out["conclusive"] = conclusive;
    out["inconclusive"] = static_cast<std::int64_t>(entries.size()) - conclusive;
    out["inconclusive_reasons"] = reasons;
    out["shots_d_gt_0"] = shots_gt_0;
    out["shots_d_gt_1cm"] = shots_gt_1cm;
    out["shots_presence_mismatch"] = shots_presence;
    out["stone_classes"] = class_block;
    out["stones_d_gt_0"] = stones_gt_0;
    out["stones_d_gt_1cm"] = stones_gt_1cm;
    out["shot_max_distance_m"] = shot_distribution;
    out["stone_distance_m_where_d_gt_0"] = distribution(stone_d);
    out["outcome"] = outcome;
    out["centimetre_m"] = kCentimetre;
    return out;
}

namespace cli {

namespace {

// Options of the comparison that make no sense in the check, and the reverse.
void validate_options(const ParsedArgs& args, bool check) {
    if (check) {
        for (const char* dest : {"compiler", "trajectory_shots", "worker_command"}) {
            if (given(args, dest)) {
                std::string flag = std::string("--") + dest;
                std::replace(flag.begin(), flag.end(), '_', '-');
                throw DcDeterminismError(flag + " does not apply to --check-recording");
            }
        }
        return;
    }
    for (const char* dest : {"removal_sheet_width", "frame_stride", "limit", "engines"}) {
        if (args.has(dest)) {
            std::string flag = std::string("--") + dest;
            std::replace(flag.begin(), flag.end(), '_', '-');
            throw DcDeterminismError(flag + " applies to --check-recording only");
        }
    }
    if (!given(args, "compiler")) throw DcDeterminismError("--compiler is required (gcc or clang) unless --check-recording is given");
    if ((given(args, "record_py27") || given(args, "record_py36")) && !given(args, "trajectory_shots")) {
        throw DcDeterminismError("--record-py27/--record-py36 need --trajectory-shots");
    }
}

}  // namespace

int command_compare_pybind(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    bool check = args.flag("check_recording");
    validate_options(args, check);
    require_pybind_platform(check ? "compare pybind --check-recording" : "compare pybind");
    if (check) return command_check_recording(args);

    PybindWorker worker(args.text_or("pybind_worker_command", ""));
    std::map<std::string, PybindBinary> officials, records;
    for (const std::string& engine : pybind_engines()) {
        officials[engine] = resolve_pybind_binary(engine, "official", args.text_or("official_" + engine, ""));
    }
    bool with_trajectories = given(args, "trajectory_shots");
    if (with_trajectories) {
        for (const std::string& engine : pybind_engines()) {
            records[engine] = resolve_pybind_binary(engine, "record", args.text_or("record_" + engine, ""));
        }
    }
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
    double timeout = args.real("timeout");
    std::vector<std::int64_t> traj_positions;
    if (with_trajectories) {
        std::string source_name = args.text_or("trajectory_shots", "");
        traj_positions = positions_of_ids(read_shot_ids(source_name), shots, source_name);
    }
    auto started = Clock::now();

    fs::path logs_root = resolve(path_from_utf8(args.text_or("logs_dir", "")));
    std::unique_ptr<Catalogue> catalogue = rebuild_catalogue(logs_root, prepared);
    ShotSource source(*catalogue);

    // Path A
    std::vector<std::string> command = target.prefix;
    for (const std::string& part :
         {std::string("--loader"), path_text(loader.path), std::string("--engine"), std::string("fcv1"),
          std::string("--input"), path_text(prepared.bin_path), std::string("--max-steps"),
          std::to_string(max_steps)}) {
        command.push_back(part);
    }
    FinalsObserver observer(total);
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
            log("path A shots " + std::to_string(done) + "/" + std::to_string(total));
        }
    };
    log("path A: " + std::to_string(total) + " shots with " + std::to_string(workers) + " workers");
    RunHooks path_a_hooks;
    path_a_hooks.observer = &observer;
    std::vector<Outcome> path_a = run_repetition(command, total, workers, 0.0, progress, path_a_hooks).second;

    // Inputs for Path B
    std::vector<Json> tasks(total);  // null: the input could not be built
    Json rows = Json::array();
    for (std::size_t position = 0; position < total; ++position) {
        const Json& shot = shots[position];
        const ShotRow& located = source.locate(shot["index"].get<std::int64_t>());
        Json row = Json::object();
        row["index"] = shot["index"];
        row["shot_id"] = shot["shot_id"];
        row["fgz"] = shot["fgz"];
        row["tournament"] = tournament_of(located.ref.archive.empty() ? located.ref.member : located.ref.archive,
                                          path_text(logs_root));
        Json path_a_row = Json::object();
        path_a_row["status"] = path_a[position].status;
        path_a_row["steps"] = path_a[position].steps;
        row["path_a"] = path_a_row;
        try {
            LoadedShot item = source.load(located, &input_keep());
            Json task = normalize_shot(item);
            if (task["shot_id"] != shot["shot_id"]) {
                throw DcDeterminismError("ValueError: pybind shot id " + py_repr(task["shot_id"]) + " differs from " +
                                 py_repr(shot["shot_id"]));
            }
            tasks[position] = std::move(task);
        } catch (const DcDeterminismError& exc) {
            row["input_error"] = exc.what();
        }
        rows.push_back(std::move(row));
    }

    std::set<std::int64_t> keep(traj_positions.begin(), traj_positions.end());
    std::map<std::pair<std::string, std::int64_t>, Json> raw_finals;
    for (const std::string& engine : pybind_engines()) {
        std::vector<std::pair<std::int64_t, Json>> adapted;
        for (std::size_t position = 0; position < total; ++position) {
            if (!tasks[position].is_null()) {
                adapted.emplace_back(static_cast<std::int64_t>(position), adapt_task(tasks[position], engine));
            }
        }
        log("path B " + pybind_name(engine) + ": " + std::to_string(adapted.size()) + " shots");
        std::map<std::int64_t, Json> results =
            run_official(worker, engine, officials[engine], adapted, timeout, workers);
        adapted.clear();
        for (std::size_t position = 0; position < total; ++position) {
            Json& row = rows[position];
            auto found = results.find(static_cast<std::int64_t>(position));
            Json entry = Json::object();
            if (tasks[position].is_null()) {
                entry["status"] = "inconclusive";
                entry["reason"] = "input error";
            } else if (path_a[position].status != "stopped") {
                entry["status"] = "inconclusive";
                entry["reason"] = "path A truncated";
            } else if (found == results.end() || found->second.contains("inconclusive")) {
                entry["status"] = "inconclusive";
                entry["reason"] = found == results.end() ? Json("no result") : found->second["inconclusive"];
            } else {
                entry = compare_shot(observer.boards[position], found->second["final"]);
                if (keep.count(static_cast<std::int64_t>(position)) != 0) {
                    raw_finals[{engine, static_cast<std::int64_t>(position)}] = found->second["final"];
                }
            }
            row[engine] = std::move(entry);
        }
    }

    Json trajectories = Json::array();
    if (!traj_positions.empty()) {
        TrajectoryContext ctx;
        ctx.worker = &worker;
        ctx.records = records;
        ctx.positions = &traj_positions;
        ctx.tasks = &tasks;
        ctx.shots = &shots;
        ctx.path_a = &path_a;
        ctx.prefix = &target.prefix;
        ctx.loader = &loader;
        ctx.prepared = &prepared;
        ctx.raw_finals = &raw_finals;
        ctx.workers = workers;
        ctx.max_steps = max_steps;
        ctx.timeout = timeout;
        trajectories = trajectory_section(ctx);
    }
    source.close();

    Json inconclusive = Json::array();
    for (const Json& row : rows) {
        for (const std::string& engine : pybind_engines()) {
            if (row[engine]["status"] != "compared") {
                Json item = Json::object();
                item["shot_id"] = row["shot_id"];
                item["engine"] = engine;
                item["reason"] = row[engine]["reason"];
                inconclusive.push_back(std::move(item));
            }
        }
    }
    Json engine_sections = Json::object();
    for (const std::string& engine : pybind_engines()) engine_sections[engine] = grouped(rows, engine);  // needs the distances
    Json sections = Json::object();
    sections["engines"] = engine_sections;
    for (Json& row : rows) {
        for (const std::string& engine : pybind_engines()) {
            if (row[engine]["status"] == "compared") row[engine].erase("distances");
        }
    }

    Json contract = Json::object();
    contract["path_a_engine"] = "fcv1";
    contract["observation"] = kObservation;
    contract["max_steps"] = max_steps;
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = document["corpus"]["input_sha256"];
    contract["loader_sha256"] = loader.info["sha256"];
    Json official_digests = Json::object(), record_digests = Json::object();
    for (const auto& item : officials) official_digests[item.first] = item.second.sha256;
    for (const auto& item : records) record_digests[item.first] = item.second.sha256;
    contract["pybind_official"] = official_digests;
    contract["pybind_record"] = record_digests;

    Json official_paths = Json::object(), record_paths = Json::object();
    for (const auto& item : officials) official_paths[item.first] = path_text(item.second.path);
    for (const auto& item : records) record_paths[item.first] = path_text(item.second.path);
    Json pybind_paths = Json::object();
    pybind_paths["official"] = official_paths;
    pybind_paths["record"] = record_paths;
    Json provenance = Json::object();
    provenance["loader"] = loader.info;
    provenance["worker"] = target.worker;
    provenance["preparer"] = document["preparer"];
    provenance["pybind_worker"] = worker.info();
    provenance["pybind_paths"] = pybind_paths;
    provenance["input_normalization"] = input_normalization();
    provenance["python_version"] = worker.python_version();
    provenance["numpy_version"] = worker.numpy_version();
    provenance["environment"] = environment::collect();

    Json known = Json::array();
    for (const char* text : kKnownDifferences) known.push_back(text);
    Json worker_counts = Json::object();
    worker_counts["requested"] = requested_workers;
    worker_counts["effective"] = workers;

    Json report = Json::object();
    report["schema"] = kSchema;
    report["contract"] = contract;
    report["provenance"] = provenance;
    report["known_implementation_differences"] = known;
    report["thresholds_m"] = thresholds_json();
    report["timeout_s"] = timeout;
    report["workers"] = worker_counts;
    report["wall_s"] = std::chrono::duration<double>(Clock::now() - started).count();
    report["inconclusive"] = inconclusive;
    report["sections"] = sections;
    report["trajectories"] = trajectories;
    report["trajectory_note"] =
        "recording frames are physical frames; frame k is the state after k steps (frame 0 = initial state) on "
        "both sides; the metrics are those of `compare simulator` on capture archives (six fields per stone)";
    report["shots"] = std::move(rows);
    write_json_new(output, report);
    for (const std::string& engine : pybind_engines()) {
        const Json& summary = report["sections"]["engines"][engine]["all"];
        log(pybind_name(engine) + ": conclusive " + std::to_string(summary["conclusive"].get<std::int64_t>()) + "/" +
            std::to_string(summary["shots"].get<std::int64_t>()) + ", d>0 shots " +
            std::to_string(summary["shots_d_gt_0"].get<std::int64_t>()) + ", d>1cm " +
            std::to_string(summary["shots_d_gt_1cm"].get<std::int64_t>()) + ", presence " +
            std::to_string(summary["shots_presence_mismatch"].get<std::int64_t>()) + ", score changes " +
            std::to_string(summary["outcome"]["scores_changed"].get<std::int64_t>()));
    }
    log("compare pybind done; wrote " + path_text(output));
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
