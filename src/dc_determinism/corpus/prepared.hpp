// Prepared corpus: `prepared.bin` (worker input) and `prepared.json`
// (identity and shot list), plus the logic that rebuilds the catalogue behind a prepared
// directory (`rebuild_catalogue`).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/platform.hpp"
#include "corpus/capi_initial.hpp"
#include "corpus/catalogue.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kPreparedMagic;   // "DCSTIN01"
constexpr std::size_t kPreparedHeaderSize = 12;
constexpr std::size_t kRecordHeadSize = 8;  // "<If"
constexpr std::size_t kRecordSize = kRecordHeadSize + kCoordSize;
extern const char* const kPreparedSchema;  // "dc-determinism-prepared-v1"
extern const char* const kPreparedBinName;
extern const char* const kPreparedJsonName;

struct PreparedRecord {
    std::uint32_t index = 0;
    float width = 0.0f;
    Coordinate coord;
};

std::string encode_record(std::uint32_t index, double width, const Coordinate& coord);
std::string encode_prepared(const std::vector<PreparedRecord>& records);
// Throws DcDeterminismError("bad magic" / "size does not match the record count").
std::vector<PreparedRecord> decode_prepared(const std::string& data);

struct PreparedCorpus {
    Json document;     // prepared.json
    fs::path bin_path; // resolved path of prepared.bin
};

// `load_prepared`: loads and verifies a prepared directory.
PreparedCorpus load_prepared(const fs::path& directory);

// `recorded_limit(corpus)`: the discovery limit that rebuilds this corpus' catalogue.
std::int64_t recorded_limit(const Json& corpus);

// `rebuild_catalogue`: re-catalogues `logs_dir` with the recorded limit; it must reproduce the
// prepared corpus (digest, count, index/shot_id/fgz list), else DcDeterminismError. The result serves
// the log comparison (ShotSource).
std::unique_ptr<Catalogue> rebuild_catalogue(const fs::path& logs_dir, const PreparedCorpus& prepared);

// `prepare_corpus`: freezes the corpus into `output` (never overwrites) and returns the
// prepared.json document. `tool_identity` is recorded as `preparer.dc_determinism` (the C++ tool's own
// identity: the hash of the tool's sources and binary). `catalogue` (optional) receives the
// catalogue for the caller.
Json prepare_corpus(const fs::path& logs_dir, std::int64_t limit,
                    const fs::path& output, const Json& tool_identity,
                    std::unique_ptr<Catalogue>* catalogue = nullptr);

}  // namespace dc_determinism
