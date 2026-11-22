// `compare simulator` for two `run simulator` result JSON files. Two capture archives (ZIP) are compared by
// `capture/compare_traj.hpp`.
#pragma once

#include <string>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kComparisonSchema;  // "dc-determinism-comparison-v2"

// `load_result`: reads and validates a result file; DcDeterminismError("invalid result <path>: ...").
Json load_result(const std::string& path);

// `compare_results`: the comparison report (`sha256_a` / `sha256_b` are the SHA-256 digests of the
// two input files); DcDeterminismError("incompatible results: ...").
Json compare_results(const Json& a, const Json& b, const std::string& sha256_a, const std::string& sha256_b);

// `zipfile.is_zipfile(path)`.
bool is_archive(const fs::path& path);

}  // namespace dc_determinism
