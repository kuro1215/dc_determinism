#include "cli/commands/player.hpp"
#include "cli/commands/player_common.hpp"
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

enum class Kind { Pdet, Result };

// The kind of an input is decided by its content, not its name.
Kind classify(const std::string& path, const std::string& bytes) {
    if (bytes.compare(0, 8, "PDET0001") == 0) return Kind::Pdet;
    if (is_player_result_document(bytes)) return Kind::Result;
    throw DcDeterminismError(path + " is neither a player output (PDET0001) nor a `run player` result JSON");
}

Json input_entry(const std::string& sha256) {
    Json input = Json::object();
    input["sha256"] = sha256;
    return input;
}

int compare_outputs(const std::string& left_path, std::string left_bytes, const std::string& right_path,
                    std::string right_bytes, const fs::path& output) {
    log("reading left result");
    PdetRecord left = decode_bytes(std::move(left_bytes), left_path);
    log("reading right result");
    PdetRecord right = decode_bytes(std::move(right_bytes), right_path);
    log("comparing results");
    Json metrics = compare_decoded(left, right);
    bool equal = metrics["different_samples"].get<std::int64_t>() == 0;

    Json inputs = Json::array();
    inputs.push_back(input_entry(left.sha256));
    inputs.push_back(input_entry(right.sha256));
    Json report = Json::object();
    report["schema"] = kPlayerCompareSchema;
    report["inputs"] = inputs;
    report["equal"] = equal;
    report["comparison"] = metrics;
    write_json_new(output, report);
    if (!equal) log("completed with differences");
    return equal ? 0 : 1;
}

Json result_entry(const PlayerResult& result) {
    Json input = input_entry(result.file_sha256);
    input["player"] = result.player;
    input["compiler"] = result.compiler;
    input["samples_per_block"] = result.samples_per_block;
    input["worker_sha256"] = result.worker_sha256;
    input["binary_origin"] = result.origin;
    return input;
}

int compare_results(const std::string& left_path, const std::string& left_bytes, const std::string& right_path,
                    const std::string& right_bytes, const fs::path& output) {
    log("reading left result");
    PlayerResult left = parse_player_result(left_bytes, left_path);
    log("reading right result");
    PlayerResult right = parse_player_result(right_bytes, right_path);
    if (left.player != right.player) {
        throw DcDeterminismError("cannot compare player results: different players (" + left.player + " and " + right.player +
                         ")");
    }
    if (left.samples_per_block != right.samples_per_block) {
        throw DcDeterminismError("cannot compare player results: different sample sizes (" +
                         std::to_string(left.samples_per_block) + " and " + std::to_string(right.samples_per_block) +
                         " per block)");
    }
    log("comparing results");
    bool same_binary = !left.worker_sha256.is_null() && left.worker_sha256 == right.worker_sha256 &&
                       left.sidecars == right.sidecars;
    bool equal = left.reproducible && right.reproducible && left.output_sha256 == right.output_sha256;

    Json inputs = Json::array();
    inputs.push_back(result_entry(left));
    inputs.push_back(result_entry(right));
    Json compatible = Json::object();
    compatible["player"] = left.player;
    compatible["samples_per_block"] = left.samples_per_block;
    Json reproducible = Json::object();
    reproducible["left"] = left.reproducible;
    reproducible["right"] = right.reproducible;
    Json hashes = Json::object();
    hashes["left"] = left.output_sha256;
    hashes["right"] = right.output_sha256;

    Json report = Json::object();
    report["schema"] = kPlayerResultCompareSchema;
    report["inputs"] = inputs;
    report["classification"] = same_binary ? "same-binary" : "cross-binary";
    report["compatible"] = compatible;
    report["reproducible"] = reproducible;
    report["output_sha256"] = hashes;
    report["equal"] = equal;
    write_json_new(output, report);
    if (!equal) log("completed with differences");
    return equal ? 0 : 1;
}

}  // namespace

int command_compare_player(const ParsedArgs& args) {
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    std::string left_path = args.text_or("left", "");
    std::string right_path = args.text_or("right", "");
    std::string left_bytes = read_file(path_from_utf8(left_path));
    std::string right_bytes = read_file(path_from_utf8(right_path));
    Kind left_kind = classify(left_path, left_bytes);
    Kind right_kind = classify(right_path, right_bytes);
    if (left_kind != right_kind) {
        throw DcDeterminismError("cannot compare a player output (.bin) with a `run player` result JSON: " + left_path +
                         " and " + right_path);
    }
    if (left_kind == Kind::Pdet) {
        return compare_outputs(left_path, std::move(left_bytes), right_path, std::move(right_bytes), output);
    }
    return compare_results(left_path, left_bytes, right_path, right_bytes, output);
}

}  // namespace cli
}  // namespace dc_determinism