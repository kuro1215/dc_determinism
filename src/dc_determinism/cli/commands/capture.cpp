#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "capture/archive.hpp"
#include "capture/collector.hpp"
#include "capture/dctr.hpp"
#include "cli/commands/capture.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/tempdir.hpp"
#include "common/text.hpp"
#include "corpus/prepared.hpp"
#include "corpus/pytext.hpp"
#include "json/writer.hpp"
#include "pybind/pybind.hpp"
#include "sim/compare.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {
namespace cli {

namespace {

const char* const kObservation = "raw-stonecoordinate-v1";
const char* const kReportSchemas[] = {"dc-determinism-comparison-v2", "dc-determinism-capture-comparison-v2"};
const char* const kReportContractKeys[] = {"observation", "record_digest", "corpus_input_sha256"};
const char* const kResultContractKeys[] = {"engine", "observation", "max_steps", "record_digest",
                                           "corpus_input_sha256"};
constexpr double kProgressIntervalSeconds = 2.0;

using Clock = std::chrono::steady_clock;

bool given(const ParsedArgs& args, const char* dest) { return args.has(dest) && !args.text_or(dest, "").empty(); }

// `[line.strip() for line in text.splitlines() if line.strip()]`.
std::vector<std::string> shot_id_lines(const std::string& text) {
    std::vector<std::string> lines;
    for (const std::string& line : py_splitlines(text)) {
        std::string stripped = py_strip(line);
        if (!stripped.empty()) lines.push_back(stripped);
    }
    return lines;
}
// `read_shot_source`: a JSON comparison report (content starting with `{`) or a shot-id list.
struct ShotListSource {
    bool report = false;
    Json document;
    std::vector<std::string> ids;
};

ShotListSource read_shot_source(const std::string& path) {
    std::string text;
    try {
        text = read_file(path_from_utf8(path));
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError(exc.what());
    }
    if (!is_valid_utf8(text)) throw DcDeterminismError("cannot read " + path + ": invalid UTF-8");
    ShotListSource out;
    std::size_t start = 0;
    while (start < text.size() && (text[start] == ' ' || text[start] == '\t' || text[start] == '\n' ||
                                   text[start] == '\r' || text[start] == '\v' || text[start] == '\f')) {
        ++start;
    }
    if (start < text.size() && text[start] == '{') {
        out.report = true;
        out.document = read_json(path_from_utf8(path));
    } else {
        out.ids = shot_id_lines(text);
    }
    return out;
}

// `mismatch_ids`: shots whose trajectory or final digest differs in a `compare simulator` report;
// the report must belong to the prepared corpus of this capture.
std::vector<Json> mismatch_ids(const Json& report, const std::string& path, const Json& contract) {
    bool known = false;
    if (report.is_object() && report.contains("schema")) {
        for (const char* schema : kReportSchemas) known = known || report["schema"] == schema;
    }
    if (!known) {
        std::string names;
        for (const char* schema : kReportSchemas) names += (names.empty() ? "" : " or ") + std::string(schema);
        throw DcDeterminismError(path + " is not a `compare simulator` report (" + names + ")");
    }
    Json recorded = json_get(report, "contract");
    if (!recorded.is_object()) throw DcDeterminismError(path + " has no contract");
    for (const char* key : kReportContractKeys) {
        Json value = json_get(recorded, key);
        if (value != contract[key]) {
            throw DcDeterminismError(path + " does not match this capture: contract " + key + " differs (" +
                             py_repr(value) + " vs " + py_repr(contract[key]) + ")");
        }
    }
    std::vector<Json> ids;
    for (const char* mode : {"trajectory", "final"}) {
        Json section = json_get(report, mode);
        Json listed = json_get(section, "mismatch_shots");
        if (!section.is_object() || !listed.is_array()) throw DcDeterminismError(path + " lacks " + mode + ".mismatch_shots");
        for (const Json& item : listed) ids.push_back(item);
    }
    std::vector<Json> unique;  // dict.fromkeys(ids): the first of equal ids
    for (const Json& id : ids) {
        if (std::find(unique.begin(), unique.end(), id) == unique.end()) unique.push_back(id);
    }
    return unique;
}

std::vector<std::int64_t> positions_of(const std::vector<Json>& ids, const Json& shots, const std::string& source) {
    std::unordered_map<std::string, std::int64_t> lookup;
    for (std::size_t position = 0; position < shots.size(); ++position) {
        lookup[shots[position]["shot_id"].get<std::string>()] = static_cast<std::int64_t>(position);
    }
    std::vector<const Json*> unknown;
    for (const Json& id : ids) {
        if (!id.is_string() || lookup.count(id.get<std::string>()) == 0) unknown.push_back(&id);
    }
    if (!unknown.empty()) {
        throw DcDeterminismError(source + " lists " + std::to_string(unknown.size()) +
                         " shots that are not in the prepared corpus, e.g. " + py_repr(*unknown[0]));
    }
    std::vector<std::int64_t> positions;
    for (const Json& id : ids) positions.push_back(lookup[id.get<std::string>()]);
    std::sort(positions.begin(), positions.end());
    positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
    return positions;
}

// `check_expect`: the captured digests against an earlier result of the same contract and loader.
Json check_expect(const std::string& path, const Json& contract, const std::string& loader_sha256,
                  const std::vector<Json>& rows) {
    Json expected = load_result(path);
    for (const char* key : kResultContractKeys) {
        if (expected["contract"][key] != contract[key]) {
            throw DcDeterminismError("--expect " + path + " is incompatible: contract " + key + " differs (" +
                             py_repr(expected["contract"][key]) + " vs " + py_repr(contract[key]) + ")");
        }
    }
    if (expected["contract"]["loader_sha256"] != Json(loader_sha256)) {
        throw DcDeterminismError("--expect " + path + " is incompatible: it was produced with another loader");
    }
    std::unordered_map<std::string, const Json*> by_id;
    for (const Json& row : expected["shots"]) by_id[row["shot_id"].get<std::string>()] = &row;
    std::vector<std::string> missing;
    for (const Json& row : rows) {
        if (by_id.count(row["shot_id"].get<std::string>()) == 0) missing.push_back(row["shot_id"].get<std::string>());
    }
    if (!missing.empty()) {
        throw DcDeterminismError("--expect " + path + " lacks " + std::to_string(missing.size()) +
                         " captured shots, e.g. " + py_repr(Json(missing[0])));
    }
    Json bad = Json::array();
    for (const Json& row : rows) {
        const Json& other = *by_id[row["shot_id"].get<std::string>()];
        if (row["trajectory_digest"] != other["trajectory_digest"] || row["final_digest"] != other["final_digest"]) {
            bad.push_back(row["shot_id"]);
        }
    }
    Json out = Json::object();
    out["path"] = path_text(resolve(path_from_utf8(path)));
    out["sha256"] = sha256_file(path_from_utf8(path));
    out["checked"] = static_cast<std::int64_t>(rows.size());
    out["equal"] = bad.empty();
    out["mismatch_shots"] = bad;
    return out;
}

std::function<void(std::size_t)> make_capture_progress(std::size_t total) {
    struct State {
        double last = 0.0;
        long long last_count = -1;
        Clock::time_point origin = Clock::now();
        bool reported = false;
    };
    auto state = std::make_shared<State>();
    return [=](std::size_t done) {
        double now = std::chrono::duration<double>(Clock::now() - state->origin).count();
        if (static_cast<long long>(done) != state->last_count &&
            (done >= total || !state->reported || now - state->last >= kProgressIntervalSeconds)) {
            state->last = now;
            state->last_count = static_cast<long long>(done);
            state->reported = true;
            log("captured shots " + std::to_string(done) + "/" + std::to_string(total));
        }
    };
}

// What the legacy input of a shot needs of its log.
const JsonKeepTree& pybind_input_keep() {
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

// `capture_pybind`: the v2 recording binary of the engine (`pybind-py27` / `pybind-py36`), frame k = the state
// after k steps.
void capture_pybind(const ParsedArgs& args, const std::string& engine_option, const PreparedCorpus& prepared,
                    const std::vector<std::int64_t>& positions, CaptureWriter& writer,
                    const std::function<void(std::size_t)>& progress, std::vector<Json>& rows, Json& provenance) {
    std::string engine = engine_option.substr(7);  // py27 / py36
    PybindWorker worker(args.text_or("pybind_worker_command", ""));
    PybindBinary binary = resolve_pybind_binary(engine, "record", "");
    const Json& document = prepared.document;
    const Json& shots = document["shots"];
    std::vector<double> widths = read_widths(prepared, shots.size());
    std::vector<std::pair<std::int64_t, Json>> items;
    {
        std::unique_ptr<Catalogue> catalogue = rebuild_catalogue(path_from_utf8(args.text_or("logs_dir", "")), prepared);
        ShotSource source(*catalogue);
        for (std::int64_t position : positions) {
            const Json& shot = shots[static_cast<std::size_t>(position)];
            Json task;
            try {
                LoadedShot item = source.load(source.locate(shot["index"].get<std::int64_t>()), &pybind_input_keep());
                task = normalize_shot(item);
                if (task["shot_id"] != shot["shot_id"]) {
                    throw DcDeterminismError("ValueError: pybind shot id " + py_repr(task["shot_id"]) + " differs from " +
                                     py_repr(shot["shot_id"]));
                }
            } catch (const DcDeterminismError& exc) {
                throw DcDeterminismError("shot " + shot["shot_id"].get<std::string>() +
                                 ": cannot build the pybind input: " + exc.what());
            }
            Json adapted = adapt_task(task, engine);
            adapted["removal_sheet_width"] = widths[static_cast<std::size_t>(position)];
            items.emplace_back(position, std::move(adapted));
        }
        source.close();
    }

    std::map<std::int64_t, Json> entries;
    std::mutex entries_mutex;
    auto consume = [&](std::int64_t position, const Json& row, std::vector<float>&& frames) {
        const Json& shot = shots[static_cast<std::size_t>(position)];
        std::string payload;
        std::int64_t steps = 0;
        std::int64_t frame_count = static_cast<std::int64_t>(frames.size() / (kFrameSlots * kFrameFields));
        try {
            payload = frames_to_payload(frames, frame_count, shot["shot_id"].get<std::string>(),
                                        shot["index"].get<std::int64_t>(), engine_option);
            std::string reason;
            if (!row.contains("steps") || !py_int(row["steps"], steps, reason)) {
                throw DcDeterminismError(reason.empty() ? std::string("missing 'steps'") : reason);
            }
        } catch (const DcDeterminismError& exc) {
            throw DcDeterminismError("invalid recording of " + shot["shot_id"].get<std::string>() + ": " + exc.what());
        }
        std::string name = capture_member_name(shot["index"].get<std::int64_t>());
        writer.add(name, payload);
        Json entry = Json::object();
        entry["index"] = shot["index"];
        entry["shot_id"] = shot["shot_id"];
        entry["status"] = "stopped";
        entry["steps"] = steps;
        entry["trajectory_digest"] = nullptr;
        entry["final_digest"] = nullptr;
        entry["frames"] = frame_count;
        entry["member"] = name;
        std::lock_guard<std::mutex> lock(entries_mutex);
        entries[position] = std::move(entry);
    };
    if (!items.empty()) {
        int workers = static_cast<int>(std::max<std::int64_t>(
            1, std::min<std::int64_t>(args.integer("workers"), static_cast<std::int64_t>(items.size()))));
        record_trajectories(worker, engine, binary, items, args.real("timeout"), workers, consume, progress);
    }
    for (std::int64_t position : positions) rows.push_back(entries.at(position));

    Json engine_binary = Json::object();
    engine_binary["kind"] = "record-v2";
    engine_binary["engine"] = pybind_name(engine);
    engine_binary["path"] = path_text(binary.path);
    engine_binary["sha256"] = binary.sha256;
    engine_binary["manifest_entry"] = manifest_entry(engine, "record");
    provenance["engine_binary"] = engine_binary;
    provenance["pybind_worker"] = worker.info();
    provenance["input_normalization"] = input_normalization();
    provenance["preparer"] = document["preparer"];
    provenance["environment"] = environment::collect();
    provenance["python_version"] = worker.python_version();
    provenance["numpy_version"] = worker.numpy_version();
}

}  // namespace

std::vector<std::int64_t> positions_of_ids(const std::vector<std::string>& ids, const Json& shots,
                                           const std::string& source) {
    std::vector<Json> values;
    for (const std::string& id : ids) values.push_back(id);
    return positions_of(values, shots, source);
}

int command_capture_simulator(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    std::string engine = args.text_or("engine", "");
    bool pybind = engine.compare(0, 7, "pybind-") == 0;
    if (pybind) {
        for (const char* dest : {"compiler", "expect", "worker_command"}) {
            if (given(args, dest)) {
                std::string flag = std::string("--") + dest;
                std::replace(flag.begin(), flag.end(), '_', '-');
                throw DcDeterminismError(flag + " applies to the C API engines only, not to " + engine);
            }
        }
        if (!given(args, "logs_dir")) {
            throw DcDeterminismError("--engine " + engine + " requires --logs-dir (the logs behind --prepared)");
        }
        require_pybind_platform("capture --engine " + engine);
    } else {
        if (!given(args, "compiler")) throw DcDeterminismError("--engine " + engine + " requires --compiler");
        if (given(args, "logs_dir")) throw DcDeterminismError("--logs-dir applies to the pybind engines only");
    }
    PreparedCorpus prepared = load_prepared(path_from_utf8(args.text_or("prepared", "")));
    const Json& document = prepared.document;
    const Json& shots = document["shots"];
    std::int64_t max_steps = args.integer("max_steps");

    Json contract = Json::object();
    contract["engine"] = engine;
    contract["observation"] = kObservation;
    contract["max_steps"] = max_steps;
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = document["corpus"]["input_sha256"];

    // `select_positions`
    std::vector<std::int64_t> positions;
    Json selection = Json::object();
    if (args.flag("all")) {
        for (std::size_t i = 0; i < shots.size(); ++i) positions.push_back(static_cast<std::int64_t>(i));
        selection["mode"] = "all";
        selection["source"] = nullptr;
    } else {
        std::string source = args.text_or("shots", "");
        ShotListSource content = read_shot_source(source);
        std::vector<Json> ids;
        if (content.report) {
            ids = mismatch_ids(content.document, source, contract);
        } else {
            for (const std::string& id : content.ids) ids.push_back(id);
        }
        positions = positions_of(ids, shots, source);
        selection["mode"] = "shots";
        selection["source_kind"] = content.report ? "compare-report" : "shot-list";
        selection["source"] = path_text(resolve(path_from_utf8(source)));
        selection["source_sha256"] = sha256_file(path_from_utf8(source));
    }
    selection["count"] = static_cast<std::int64_t>(positions.size());
    log("capturing " + std::to_string(positions.size()) + " of " + std::to_string(shots.size()) + " shots with " +
        engine);

    CaptureWriter writer(output);

    std::size_t total = positions.size();
    auto progress = make_capture_progress(total);
    std::vector<Json> rows;
    Json expect = nullptr;
    Json provenance = Json::object();
    if (pybind) {
        capture_pybind(args, engine, prepared, positions, writer, progress, rows, provenance);
    } else {
        // `capture_capi`: the C API engines through the worker's `T` requests.
        SimulatorTarget target = resolve_simulator(args.text_or("compiler", ""), args.text_or("worker_command", ""));
        const ResolvedLoader& loader = target.loader;
        std::map<std::int64_t, Json> entries;
        std::mutex entries_mutex;

        auto sink = [&](std::int64_t position, const Outcome& outcome, std::string&& payload, std::int64_t frames) {
            const Json& shot = shots[static_cast<std::size_t>(position)];
            std::string name = capture_member_name(shot["index"].get<std::int64_t>());
            writer.add(name, payload);
            Json entry = Json::object();
            entry["index"] = shot["index"];
            entry["shot_id"] = shot["shot_id"];
            entry["status"] = outcome.status;
            entry["steps"] = outcome.steps;
            entry["trajectory_digest"] = outcome.trajectory_digest;
            entry["final_digest"] = outcome.final_digest;
            entry["frames"] = frames;
            entry["member"] = name;
            std::lock_guard<std::mutex> lock(entries_mutex);
            entries[position] = std::move(entry);
        };

        if (total != 0) {
            TempDir tmp("dc_determinism-capture-");
            std::vector<std::string> command = target.prefix;
            for (const std::string& part :
                 {std::string("--loader"), path_text(loader.path), std::string("--engine"), engine, std::string("--input"),
                  path_text(prepared.bin_path), std::string("--max-steps"), std::to_string(max_steps),
                  std::string("--capture-dir"), path_text(tmp.path())}) {
                command.push_back(part);
            }
            TrajectoryCollector collector(tmp.path(), engine, shots, sink);
            int workers = static_cast<int>(std::max<std::int64_t>(
                1, std::min<std::int64_t>(args.integer("workers"), static_cast<std::int64_t>(total))));
            RunHooks hooks;
            hooks.observer = &collector;
            hooks.positions = &positions;
            run_repetition(command, total, workers, args.real("timeout"), progress, hooks);
        }

        for (std::int64_t position : positions) rows.push_back(entries.at(position));
        if (given(args, "expect")) {
            expect = check_expect(args.text_or("expect", ""), contract, loader.info["sha256"].get<std::string>(), rows);
        }
        provenance["loader"] = loader.info;
        provenance["worker"] = target.worker;
        provenance["preparer"] = document["preparer"];
        provenance["environment"] = environment::collect();
    }

    Json index = Json::object();
    index["schema"] = kCaptureSchema;
    index["format"] = kDctrFormat;
    index["contract"] = contract;
    index["selection"] = selection;
    index["provenance"] = provenance;
    Json row_list = Json::array();
    for (Json& row : rows) row_list.push_back(std::move(row));
    index["shots"] = row_list;
    if (!expect.is_null()) index["expect"] = expect;
    writer.commit(index);

    log("capture done: " + std::to_string(positions.size()) + " shots; wrote " + path_text(output));
    if (given(args, "expect")) {
        if (expect["equal"].get<bool>()) {
            log("expect: " + std::to_string(expect["checked"].get<std::int64_t>()) + " shots equal");
        } else {
            std::string text = "expect: DIFFERENT for " + std::to_string(expect["mismatch_shots"].size()) + " shots: ";
            std::size_t limit = std::min<std::size_t>(20, expect["mismatch_shots"].size());
            for (std::size_t i = 0; i < limit; ++i) {
                if (i != 0) text += ", ";
                text += expect["mismatch_shots"][i].get<std::string>();
            }
            log(text);
        }
    }
    return expect.is_null() || expect["equal"].get<bool>() ? 0 : 1;
}

}  // namespace cli
}  // namespace dc_determinism
