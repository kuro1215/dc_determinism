// `run player` result files (`dc-determinism-player-result-v1`): detection by content and validation, for
// `compare player` and `capture player --expect`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace dc_determinism {

struct PlayerResult {
    Json document;
    std::string label;          // the path as the user gave it
    std::string file_sha256;    // SHA-256 of the file bytes
    std::string player;
    std::string compiler;
    std::int64_t samples_per_block = 0;
    Json worker_sha256;         // string or null
    Json sidecars;              // `binary.sidecars` ({} when absent)
    Json origin;                // `binary.origin` or null
    std::vector<std::string> run_hashes;
    bool reproducible = false;
    Json output_sha256;         // string, or null when the runs differ
};

// True when `bytes` are a JSON object carrying the result schema (nothing is validated).
bool is_player_result_document(const std::string& bytes);

// Parses and validates a result: DcDeterminismError "invalid player result <label>: <reason>" (or the JSON
// parse error) when it is damaged or inconsistent.
PlayerResult parse_player_result(const std::string& bytes, const std::string& label);

// Exact schema names.
extern const char* const kPlayerResultSchema;            // "dc-determinism-player-result-v1"
extern const char* const kPlayerResultCompareSchema;     // "dc-determinism-player-result-comparison-v2"

}  // namespace dc_determinism
