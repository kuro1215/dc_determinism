// What `run player`, `capture player` and `compare player` share: one fresh player process,
// decoding and checking of its PDET output, and the private scratch directory the output goes to.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "common/platform.hpp"
#include "json/json.hpp"
#include "player/pdet.hpp"

namespace dc_determinism {
namespace cli {

constexpr std::int64_t kMinSamples = 640;
constexpr std::int64_t kMaxSamples = 1000000;

// The option is present and not empty.
bool given(const ParsedArgs& args, const char* dest);

// `--samples` must be within kMinSamples..kMaxSamples.
void check_samples(std::int64_t samples);

// A new directory `<output>.tmp-<random>` next to `output` (the parent directories are created);
// removed with its content on destruction, with a warning when that fails.
class Scratch {
public:
    explicit Scratch(const fs::path& output);
    ~Scratch();
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// One fresh process: the player writes `binary` and prints its metadata on stdout. Returns the
// parsed first stdout line (null when there is none or it is not JSON).
Json run_player_once(const std::vector<std::string>& command, const fs::path& binary, std::int64_t samples,
                     double timeout);

// Decoding errors become `invalid player output <label>: ...`.
PdetRecord decode_bytes(std::string data, const std::string& label);
PdetRecord decode_file(const std::string& path_text_value);

// `cannot compare player outputs: ...`.
Json compare_decoded(const PdetRecord& left, const PdetRecord& right);

// A decoded output must belong to `player` and have `samples` per block.
void check_output(const PdetRecord& record, const std::string& label, const std::string& player, std::int64_t samples);

// One compact JSON line on stdout.
void print_line(const Json& value);

}  // namespace cli
}  // namespace dc_determinism
