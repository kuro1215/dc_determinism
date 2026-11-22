// `verify`, `run simulator` and `compare simulator` (C++ only).
#pragma once

#include <string>

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

int command_verify(const ParsedArgs& args);
int command_run_simulator(const ParsedArgs& args);
int command_compare_simulator(const ParsedArgs& args);

// `check_corpus_args`: exactly one of --logs-dir / --prepared, except when the
// logs are needed again (`needs_logs`: both allowed, --logs-dir required; `reason` names why);
// --limit only with --logs-dir.
void check_corpus_args(const ParsedArgs& args, bool needs_logs, const std::string& reason);

// `short_reason`: one-line summary of a verification failure (the first line of the message, cut
// before the detail that follows the first ': ').
std::string short_reason(const std::string& message);

}  // namespace cli
}  // namespace dc_determinism
