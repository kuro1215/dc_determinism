#include <algorithm>
#include <memory>

#include "cli/commands/prepare.hpp"
#include "cli/commands/simulator.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/tempdir.hpp"
#include "common/text.hpp"
#include "corpus/prepared.hpp"
#include "json/writer.hpp"
#include "logs/evaluator.hpp"
#include "logs/log_compare.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {
namespace cli {

namespace {

// `bool(args.<dest>)` of a string option.
bool given(const ParsedArgs& args, const char* dest) { return args.has(dest) && !args.text_or(dest, "").empty(); }

}  // namespace

// `check_corpus_args`: exactly one of --logs-dir / --prepared, except when the logs are needed
// again (`needs_logs`), where both may be given.
void check_corpus_args(const ParsedArgs& args, bool needs_logs, const std::string& reason) {
    int count = (given(args, "logs_dir") ? 1 : 0) + (given(args, "prepared") ? 1 : 0);
    if (count == 0 || (count == 2 && !needs_logs)) throw DcDeterminismError("specify exactly one of --logs-dir or --prepared");
    if (needs_logs && !given(args, "logs_dir")) {
        throw DcDeterminismError(reason + " requires --logs-dir (also together with --prepared)");
    }
    if (given(args, "prepared") && args.integer("limit") != 0) {
        throw DcDeterminismError("--limit is only valid with --logs-dir (without --prepared)");
    }
}

namespace {

const char* const kObservation = "raw-stonecoordinate-v1";

// `(timing.get("catalogue_s") or 0) + (timing.get("prepare_s") or 0)`.
Json prepare_seconds(const Json& timing) {
    auto number = [&](const char* key) {
        Json value = json_get(timing, key);
        return py_truthy(value) ? value : Json(0);
    };
    Json a = number("catalogue_s");
    Json b = number("prepare_s");
    if (a.is_number_integer() && b.is_number_integer()) return Json(a.get<std::int64_t>() + b.get<std::int64_t>());
    return Json(a.get<double>() + b.get<double>());
}

}  // namespace

int command_run_simulator(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);

    // `log_settings`: the --compare-logs options.
    bool extras = args.has("compare_logs_basis") || args.flag("compare_logs_basis_fallback") ||
                  args.has("compare_logs_tolerance");
    bool compare_logs = args.flag("compare_logs");
    if (!compare_logs && extras) {
        throw DcDeterminismError("--compare-logs-basis, --compare-logs-basis-fallback and --compare-logs-tolerance require "
                         "--compare-logs");
    }
    check_corpus_args(args, compare_logs, "--compare-logs");
    LogSettings settings;
    if (compare_logs) {
        if (args.has("compare_logs_basis")) settings.basis = args.text_or("compare_logs_basis", settings.basis);
        settings.fallback = args.flag("compare_logs_basis_fallback");
        if (args.has("compare_logs_tolerance")) settings.tolerance = args.real("compare_logs_tolerance");
    }

    std::string compiler = args.text_or("compiler", "");
    std::string engine = args.text_or("engine", "");
    std::int64_t repeats = args.integer("repeats");
    std::int64_t requested_workers = args.integer("workers");
    std::int64_t max_steps = args.integer("max_steps");
    double timeout = args.real("timeout");

    SimulatorTarget target = resolve_simulator(compiler, args.text_or("worker_command", ""));
    const ResolvedLoader& loader = target.loader;
    const std::vector<std::string>& prefix = target.prefix;

    std::vector<double> walls;
    std::vector<std::vector<Outcome>> runs;
    TempDir tmp("dc_determinism-run-");
    PreparedCorpus prepared;
    std::unique_ptr<Catalogue> catalogue;  // only with --compare-logs
    bool reused;
    if (given(args, "prepared")) {
        prepared = load_prepared(path_from_utf8(args.text_or("prepared", "")));
        reused = true;
        if (compare_logs) catalogue = rebuild_catalogue(path_from_utf8(args.text_or("logs_dir", "")), prepared);
    } else {
        fs::path prep_dir = tmp.path() / "prepared";
        prepare_corpus(path_from_utf8(args.text_or("logs_dir", "")), args.integer("limit"), prep_dir, tool_identity(),
                       compare_logs ? &catalogue : nullptr);
        prepared = load_prepared(prep_dir);
        reused = false;
    }
    const Json& document = prepared.document;
    const Json& shots = document["shots"];
    std::size_t total = shots.size();
    int workers = static_cast<int>(std::max<std::int64_t>(
        1, std::min<std::int64_t>(requested_workers, static_cast<std::int64_t>(total))));
    const Json& corpus = document["corpus"];
    Json contract = Json::object();
    contract["engine"] = engine;
    contract["observation"] = kObservation;
    contract["max_steps"] = max_steps;
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = corpus["input_sha256"];
    contract["loader_sha256"] = loader.info["sha256"];

    std::vector<std::string> command = prefix;
    for (const std::string& part : {std::string("--loader"), path_text(loader.path), std::string("--engine"), engine,
                                    std::string("--input"), path_text(prepared.bin_path), std::string("--max-steps"),
                                    std::to_string(max_steps)}) {
        command.push_back(part);
    }

    std::string sim_bits;
    double sim_spf = 0.0;
    std::unique_ptr<LogEvaluator> evaluator;
    std::vector<FrameInfo> infos;
    std::vector<Json> metrics;
    if (compare_logs) {
        std::pair<std::string, double> sim_info = query_seconds_per_frame(prefix, path_text(loader.path), engine);
        sim_bits = sim_info.first;
        sim_spf = sim_info.second;
        evaluator.reset(new LogEvaluator(*catalogue, shots, sim_spf, settings, workers));
        log("reading recorded trajectories ...");
        infos = evaluator->frame_infos();
    }
    for (std::int64_t repeat = 1; repeat <= repeats; ++repeat) {
        log("repetition " + std::to_string(repeat) + "/" + std::to_string(repeats) + ": " + std::to_string(workers) +
            " workers");
        std::unique_ptr<LogObserver> observer;
        RunHooks hooks;
        if (evaluator && repeat == 1) {
            observer = evaluator->observer(infos);
            hooks.observer = observer.get();
        }
        std::pair<double, std::vector<Outcome>> run =
            run_repetition(command, total, workers, timeout, make_progress(static_cast<int>(repeat), repeats, total),
                           hooks);
        if (observer) metrics = observer->metrics();
        log("repetition " + std::to_string(repeat) + "/" + std::to_string(repeats) + ": wall " +
            format_fixed(run.first, 2) + " s");
        walls.push_back(run.first);
        runs.push_back(std::move(run.second));
    }

    Json rows = reduce_shots(shots, runs);
    if (compare_logs) attach_log_entries(rows, metrics, settings);
    bool reproducible = true;
    std::int64_t total_steps = 0;
    std::size_t bad = 0;
    for (const Json& row : rows) {
        bool good = row["reproducible_trajectory"].get<bool>() && row["reproducible_final"].get<bool>() &&
                    row["all_stopped"].get<bool>();
        reproducible = reproducible && good;
        if (!good) ++bad;
        total_steps += row["steps"].get<std::int64_t>();
    }
    Json timing = Json::object();
    timing["prepare_s"] = prepare_seconds(json_get(document, "timing"));
    timing["prepared_reused"] = reused;
    Json wall_list = Json::array();
    for (double wall : walls) wall_list.push_back(wall);
    timing["wall_s"] = wall_list;
    Json core = Json::array();
    for (const auto& run : runs) {
        std::int64_t sim_ns = 0;
        for (const Outcome& outcome : run) sim_ns += outcome.sim_ns;
        core.push_back(static_cast<double>(sim_ns) / 1e9);
    }
    timing["core_simulate_s"] = core;
    timing["total_steps"] = total_steps;

    Json worker_counts = Json::object();
    worker_counts["requested"] = requested_workers;
    worker_counts["effective"] = workers;

    Json result = Json::object();
    result["schema"] = kEvaluationSchema;
    result["contract"] = contract;
    result["loader"] = loader.info;
    result["worker"] = target.worker;
    result["preparer"] = document["preparer"];
    result["environment"] = environment::collect();
    result["repeats"] = repeats;
    result["workers"] = worker_counts;
    result["skipped_concede"] = json_get(corpus, "skipped_concede");
    result["timing"] = timing;
    result["shots"] = rows;
    result["reproducible"] = reproducible;
    if (compare_logs) {
        Json summary = log_summary(rows, settings, sim_bits, sim_spf);
        summary["dc_determinism"] = tool_identity();
        result["log_comparison"] = summary;
    }
    write_json_new(output, result);
    log("done: " + std::to_string(total) + " shots, " + std::to_string(repeats) +
        " repetitions, problem shots=" + std::to_string(bad) + "; wrote " + path_text(output));
    return reproducible ? 0 : 1;
}

}  // namespace cli
}  // namespace dc_determinism
