#include <optional>

#include "cli/commands/player.hpp"
#include "cli/commands/player_common.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "player/binaries.hpp"
#include "player/pdet.hpp"
#include "player/result.hpp"
#include "sim/metrics.hpp"

namespace dc_determinism {
namespace cli {

namespace {

// `--expect`: a reproducible `run player` result of the same player, compiler and sample size.
PlayerResult load_expected(const std::string& path_given, const std::string& player, const std::string& compiler,
                           std::int64_t samples) {
    PlayerResult expected = parse_player_result(read_file(path_from_utf8(path_given)), path_given);
    if (expected.player != player || expected.compiler != compiler || expected.samples_per_block != samples) {
        throw DcDeterminismError("--expect result is for " + expected.player + " / " + expected.compiler + " / " +
                         std::to_string(expected.samples_per_block) + " samples per block, not " + player + " / " +
                         compiler + " / " + std::to_string(samples));
    }
    if (!expected.reproducible) {
        throw DcDeterminismError("--expect result is not reproducible (its repetitions differ): " + path_given);
    }
    return expected;
}

}  // namespace

int command_capture_player(const ParsedArgs& args) {
    std::int64_t samples = args.integer("samples");
    check_samples(samples);
    fs::path output = resolve(path_from_utf8(args.text_or("output", "")));
    refuse_existing(output);
    std::string player = args.text_or("player", "");
    std::string compiler = args.text_or("compiler", "");
    double timeout = args.real("timeout");

    // The expectation is validated before the player starts.
    std::optional<PlayerResult> expected;
    if (given(args, "expect")) expected = load_expected(args.text_or("expect", ""), player, compiler, samples);

    PlayerSelection selection = resolve_player(player, compiler, args.text_or("player_command", ""));
    const Json& binary_info = selection.binary;

    Scratch scratch(output);
    fs::path binary_path = scratch.path() / "capture.bin";
    log("running the player");
    run_player_once(selection.command, binary_path, samples, timeout);
    if (!binary_info["path"].is_null()) recheck_binary(binary_info);
    PdetRecord record = decode_file(path_text(binary_path));
    check_output(record, "player output", player, samples);
    if (record.restore_failures != 0) throw DcDeterminismError("player storage continuation probe failed");
    commit_new(binary_path, output);
    log("wrote " + path_text(output));

    Json expect = nullptr;
    if (expected) {
        expect = Json::object();
        expect["equal"] = expected->output_sha256 == Json(record.sha256);
        if (!expect["equal"].get<bool>()) log("expect: DIFFERENT (the captured output is not the expected one)");
    }
    Json line = Json::object();
    line["output"] = path_text(output);
    line["sha256"] = record.sha256;
    line["expect"] = expect;
    print_line(line);
    return expect.is_null() || expect["equal"].get<bool>() ? 0 : 1;
}

}  // namespace cli
}  // namespace dc_determinism