#include <chrono>
#include <string>
#include <vector>

#include "cli/commands/player.hpp"
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
#include "sim/bench.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {
namespace cli {

namespace {

constexpr std::int64_t kMaxSteps = 60000;
constexpr double kProgressIntervalSeconds = 2.0;
const char* const kCapiScope = "set_stones, simulate and stopped check; excludes create and remove";

bool given(const ParsedArgs& args, const char* dest) { return args.has(dest) && !args.text_or(dest, "").empty(); }

}  // namespace

int command_benchmark(const ParsedArgs& args) {
    using Clock = std::chrono::steady_clock;
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    check_corpus_args(args, false, "");
    SimulatorTarget target = resolve_simulator(args.text_or("compiler", ""), args.text_or("worker_command", ""));
    const ResolvedLoader& loader = target.loader;

    const std::vector<std::string> engines = {"fcv1", "fcv1plus"};
    std::int64_t repeats = args.integer("repeats");
    double timeout = args.real("timeout");

    TempDir tmp("dc_determinism-bench-");
    PreparedCorpus prepared;
    if (given(args, "prepared")) {
        prepared = load_prepared(path_from_utf8(args.text_or("prepared", "")));
    } else {
        fs::path prep_dir = tmp.path() / "prepared";
        prepare_corpus(path_from_utf8(args.text_or("logs_dir", "")), args.integer("limit"), prep_dir, tool_identity(),
                       nullptr);
        prepared = load_prepared(prep_dir);
    }
    const Json& document = prepared.document;
    const Json& shots = document["shots"];
    std::size_t total = shots.size();
    std::vector<PreparedRecord> records_on_disk = decode_prepared(read_file(prepared.bin_path));

    std::vector<std::string> command = target.prefix;
    for (const std::string& part : {std::string("--loader"), path_text(loader.path), std::string("--engine"),
                                    engines[0], std::string("--input"), path_text(prepared.bin_path),
                                    std::string("--max-steps"), std::to_string(kMaxSteps), std::string("--engines"),
                                    engines[0] + "," + engines[1]}) {
        command.push_back(part);
    }
    BenchChannel channel(command);

    Json records = Json::array();
    auto started = Clock::now();
    auto last = Clock::now();
    for (std::size_t position = 0; position < total; ++position) {
        const Json& shot = shots[position];
        auto measure = [&](const std::string& engine) {
            std::string line = channel.request("B " + std::to_string(position) + " " + engine, timeout);
            return parse_bench_line(line, static_cast<std::int64_t>(position), engine);
        };
        for (const std::string& engine : engines) measure(engine);  // one untimed warm-up per engine
        std::vector<std::vector<std::int64_t>> timings(engines.size());
        Json orders = Json::array();
        for (std::int64_t repeat = 0; repeat < repeats; ++repeat) {
            std::vector<std::string> order = engine_order(engines, repeat);
            orders.push_back(order);
            for (const std::string& engine : order) {
                std::size_t slot = engine == engines[0] ? 0 : 1;
                timings[slot].push_back(measure(engine));
            }
        }
        Json record = Json::object();
        record["index"] = shot["index"];
        record["shot_id"] = shot["shot_id"];
        record["sheet_width"] = static_cast<double>(records_on_disk.at(position).width);
        record["orders"] = orders;
        Json timing_block = Json::object();
        Json medians = Json::object();
        for (std::size_t slot = 0; slot < engines.size(); ++slot) {
            std::vector<Json> values;
            for (std::int64_t value : timings[slot]) values.push_back(value);
            timing_block[engines[slot]] = timings[slot];
            medians[engines[slot]] = py_median(values);
        }
        record["timings_ns"] = timing_block;
        record["median_ns"] = medians;
        records.push_back(record);
        if (std::chrono::duration<double>(Clock::now() - last).count() >= kProgressIntervalSeconds ||
            position + 1 == total) {
            log("benchmark shot " + std::to_string(position + 1) + "/" + std::to_string(total));
            last = Clock::now();
        }
    }
    double wall = std::chrono::duration<double>(Clock::now() - started).count();
    std::optional<std::int64_t> code = channel.close();
    if (!code || *code != 0) {
        throw DcDeterminismError("worker exited with code " + (code ? std::to_string(*code) : std::string("None")));
    }

    const Json& corpus = document["corpus"];
    Json contract = Json::object();
    contract["engines"] = engines;
    contract["repeats"] = repeats;
    contract["max_steps"] = kMaxSteps;
    contract["prepared_sha256"] = document["prepared_sha256"];
    contract["record_digest"] = document["record_digest"];
    contract["corpus_input_sha256"] = corpus["input_sha256"];
    contract["loader_sha256"] = loader.info["sha256"];
    contract["pybind"] = Json::object();
    Json scopes = Json::object();
    scopes["capi"] = kCapiScope;

    Json result = Json::object();
    result["schema"] = kBenchmarkSchema;
    result["measurement_only"] = true;
    result["contract"] = contract;
    result["loader"] = loader.info;
    result["worker"] = target.worker;
    result["preparer"] = document["preparer"];
    result["environment"] = environment::collect();
    result["scopes"] = scopes;
    result["timeout_s"] = timeout;
    result["wall_s"] = wall;
    result["shots"] = records;
    result["summary"] = summarize_benchmark(records, engines);
    write_json_new(output, result);
    log("benchmark done: " + std::to_string(total) + " shots, " + std::to_string(repeats) + " repeats, engines " +
        engines[0] + ", " + engines[1] + "; wrote " + path_text(output));
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
