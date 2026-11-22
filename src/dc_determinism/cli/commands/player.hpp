// `run player`, `capture player` and `compare player` (JSON-only repetitions, a kept single output,
// and the comparison of either), and `benchmark` (serial timing of the engines on the corpus).
#pragma once

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

int command_run_player(const ParsedArgs& args);
int command_capture_player(const ParsedArgs& args);
int command_compare_player(const ParsedArgs& args);
int command_benchmark(const ParsedArgs& args);

}  // namespace cli
}  // namespace dc_determinism
