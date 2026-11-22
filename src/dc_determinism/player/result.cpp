#include "player/result.hpp"

#include <set>

#include "common/error.hpp"
#include "common/sha256.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "player/binaries.hpp"

namespace dc_determinism {

const char* const kPlayerResultSchema = "dc-determinism-player-result-v1";
const char* const kPlayerResultCompareSchema = "dc-determinism-player-result-comparison-v2";

namespace {

[[noreturn]] void invalid(const std::string& label, const std::string& reason) {
    throw DcDeterminismError("invalid player result " + label + ": " + reason);
}

}  // namespace

bool is_player_result_document(const std::string& bytes) {
    Json value = Json::parse(bytes, nullptr, false);
    return !value.is_discarded() && value.is_object() && json_get(value, "schema") == kPlayerResultSchema;
}

PlayerResult parse_player_result(const std::string& bytes, const std::string& label) {
    PlayerResult result;
    result.label = label;
    result.file_sha256 = sha256_hex(bytes);
    result.document = parse_json_text(bytes, label);
    const Json& document = result.document;
    if (!document.is_object()) invalid(label, "not a JSON object");
    if (json_get(document, "schema") != kPlayerResultSchema) invalid(label, "unknown schema");

    Json player = json_get(document, "player");
    if (!player.is_string() || (player.get<std::string>() != "normal_dist" && player.get<std::string>() != "nd2")) {
        invalid(label, "`player` is not normal_dist or nd2");
    }
    result.player = player.get<std::string>();
    Json compiler = json_get(document, "compiler");
    if (!compiler.is_string()) invalid(label, "`compiler` is not a string");
    result.compiler = compiler.get<std::string>();
    Json samples = json_get(document, "samples_per_block");
    if (!samples.is_number_integer() || samples.get<std::int64_t>() < 1) {
        invalid(label, "`samples_per_block` is not a positive integer");
    }
    result.samples_per_block = samples.get<std::int64_t>();

    Json runs = json_get(document, "runs");
    if (!runs.is_array() || runs.empty()) invalid(label, "`runs` is not a non-empty list");
    std::set<std::string> distinct;
    for (const Json& run : runs) {
        Json digest = json_get(run, "sha256");
        if (!digest.is_string() || !is_hex64(digest.get<std::string>())) {
            invalid(label, "a run has no 64-digit `sha256`");
        }
        result.run_hashes.push_back(digest.get<std::string>());
        distinct.insert(digest.get<std::string>());
    }
    result.reproducible = distinct.size() == 1;
    result.output_sha256 = result.reproducible ? Json(result.run_hashes[0]) : Json(nullptr);

    Json reproducible = json_get(document, "reproducible");
    if (!reproducible.is_boolean() || reproducible.get<bool>() != result.reproducible) {
        invalid(label, "`reproducible` does not match the runs");
    }
    if (json_get(document, "output_sha256") != result.output_sha256) {
        invalid(label, "`output_sha256` does not match the runs");
    }
    Json count = json_get(document, "distinct_output_sha256");
    if (!count.is_number_integer() || count.get<std::int64_t>() != static_cast<std::int64_t>(distinct.size())) {
        invalid(label, "`distinct_output_sha256` does not match the runs");
    }

    Json binary = json_get(document, "binary");
    if (!binary.is_null() && !binary.is_object()) invalid(label, "`binary` is not an object");
    Json worker = json_get(document, "worker_sha256");
    if (!worker.is_null() && !(worker.is_string() && is_hex64(worker.get<std::string>()))) {
        invalid(label, "`worker_sha256` is not a 64-digit hash");
    }
    result.worker_sha256 = worker;
    Json sidecars = json_get(binary, "sidecars");
    result.sidecars = sidecars.is_object() ? sidecars : Json::object();
    result.origin = json_get(binary, "origin");
    return result;
}

}  // namespace dc_determinism
