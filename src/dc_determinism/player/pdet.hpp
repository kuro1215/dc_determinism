// Canonical player outputs (`PDET0001`) and their comparison: `decode_pdet` and
// `compare_pdet`.
//
// Decoding and comparison fail with PdetError; the exact messages are those of Python's
// ValueError / struct.error, because the commands print them (`invalid player output <path>: ...`,
// `cannot compare player outputs: ...`).
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace dc_determinism {

class PdetError : public std::runtime_error {
public:
    explicit PdetError(const std::string& message) : std::runtime_error(message) {}
};

constexpr std::size_t kPdetBlocks = 256;
constexpr std::size_t kPdetBlocksPerProfile = 64;
constexpr std::size_t kPdetCallSize = 24;  // six little-endian uint32 per sample
constexpr std::size_t kPdetConfigSize = 32;
constexpr std::size_t kPdetControlsSize = 192;

struct PdetBlock {
    std::uint32_t profile = 0;
    std::uint32_t seed = 0;
    std::uint32_t n = 0;
    std::size_t config_offset = 0;    // kPdetConfigSize bytes in `data`
    std::size_t controls_offset = 0;  // kPdetControlsSize bytes
    std::size_t calls_offset = 0;     // n * kPdetCallSize bytes
};

struct PdetRecord {
    std::string data;  // the whole file
    std::uint32_t player = 0;
    std::vector<PdetBlock> blocks;
    std::int64_t samples = 0;
    std::int64_t restore_probes = 0;
    std::int64_t restore_failures = 0;
    std::string sha256;
};

// `decode(path)` of the file content.
PdetRecord decode_pdet(std::string data);

// `compare(left, right)`: the metrics as a dict (key order included).
Json compare_pdet(const PdetRecord& left, const PdetRecord& right);

}  // namespace dc_determinism
