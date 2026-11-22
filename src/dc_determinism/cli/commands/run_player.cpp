#include <cstdio>
#include <set>
#include <system_error>

#include "cli/commands/player.hpp"
#include "cli/commands/player_common.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "player/binaries.hpp"
#include "player/pdet.hpp"
#include "player/result.hpp"
#include "sim/metrics.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {
namespace cli {

namespace {

std::string repeat_stem(std::int64_t index) {
    char text[32];
    std::snprintf(text, sizeof(text), "repeat-%03lld", static_cast<long long>(index));
    return text;
}

// What stays of one repetition once its output has been hashed and checked.
struct RunRecord {
    std::string sha256;
    std::int64_t samples = 0;
    std::int64_t restore_failures = 0;
    Json metadata;
};

}  // namespace

int command_run_player(const ParsedArgs& args) {
    std::int64_t samples = args.integer("samples");
    check_samples(samples);
    fs::path output = resolve(path_from_utf8(args.text_or("output", "")));
    refuse_existing(output);
    std::string player = args.text_or("player", "");
    std::string compiler = args.text_or("compiler", "");
    std::int64_t repeats = args.integer("repeats");
    double timeout = args.real("timeout");

    PlayerSelection selection = resolve_player(player, compiler, args.text_or("player_command", ""));
    const Json& binary_info = selection.binary;

    // The repetitions' outputs live in a private directory next to the result and are deleted.
    Scratch scratch(output);
    std::vector<RunRecord> runs;
    PdetRecord first;
    std::string deferred_conflict;
    for (std::int64_t index = 0; index < repeats; ++index) {
        log("player repetition " + std::to_string(index + 1) + "/" + std::to_string(repeats));
        std::string stem = repeat_stem(index + 1);
        fs::path binary_path = scratch.path() / (stem + ".bin");
        Json metadata = run_player_once(selection.command, binary_path, samples, timeout);
        if (!binary_info["path"].is_null()) recheck_binary(binary_info);
        PdetRecord record = decode_file(path_text(binary_path));
        check_output(record, "player output " + stem + ".bin", player, samples);
        std::error_code ignored;
        fs::remove(binary_path, ignored);

        RunRecord run;
        run.sha256 = record.sha256;
        run.samples = record.samples;
        run.restore_failures = record.restore_failures;
        run.metadata = metadata;
        runs.push_back(run);
        if (index == 0) {
            first = std::move(record);
        } else if (deferred_conflict.empty()) {
            // The same block identities, configurations, controls and inputs as repetition 1; an
            // error is reported after the probe check, as when all outputs are compared at the end.
            try {
                compare_decoded(first, record);
            } catch (const DcDeterminismError& error) {
                deferred_conflict = error.what();
            }
        }
    }
    for (const RunRecord& run : runs) {
        if (run.restore_failures != 0) throw DcDeterminismError("player storage continuation probe failed");
    }
    if (!deferred_conflict.empty()) throw DcDeterminismError(deferred_conflict);

    // Source provenance is attached only when the binary is the manifest's own.
    Json section = nullptr;
    bool has_entry = !binary_info["manifest_entry"].is_null();
    if (has_entry) {
        try {
            section = json_get(players_section(), "source");
        } catch (const DcDeterminismError&) {
            section = nullptr;
        }
    }

    std::set<std::string> distinct;
    Json run_list = Json::array();
    for (std::size_t i = 0; i < runs.size(); ++i) {
        distinct.insert(runs[i].sha256);
        Json run = Json::object();
        run["index"] = static_cast<std::int64_t>(i) + 1;
        run["sha256"] = runs[i].sha256;
        run["samples"] = runs[i].samples;
        run["restore_failures"] = runs[i].restore_failures;
        run["metadata"] = runs[i].metadata;
        run_list.push_back(run);
    }
    bool reproducible = distinct.size() == 1;

    Json result = Json::object();
    result["schema"] = kPlayerResultSchema;
    result["player"] = player;
    result["compiler"] = compiler;
    result["samples_per_block"] = samples;
    result["repeats"] = repeats;
    result["source_revision"] = has_entry ? Json(kPlayerRevision) : Json(nullptr);
    result["worker_sha256"] = binary_info["sha256"];
    result["runs"] = run_list;
    result["reproducible"] = reproducible;
    result["output_sha256"] = reproducible ? Json(runs[0].sha256) : Json(nullptr);
    result["distinct_output_sha256"] = static_cast<std::int64_t>(distinct.size());
    result["binary"] = binary_info;
    result["runtime"] = selection.runtime;
    result["players_source"] = section;
    result["environment"] = environment::collect();
    write_json_new(output, result);

    Json line = Json::object();
    line["output"] = path_text(output);
    line["reproducible"] = reproducible;
    print_line(line);
    if (!reproducible) log("completed: the repetitions are not reproducible");
    return reproducible ? 0 : 1;
}

}  // namespace cli
}  // namespace dc_determinism