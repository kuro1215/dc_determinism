// Command dispatch. Each command handler takes the parsed arguments and returns the exit code
// (0/1/2); a DcDeterminismError becomes `error: <message>` and exit code 2 in main().
//
// Handlers live in cli/commands/<command>.cpp.
#pragma once

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

// Space-separated command path, e.g. "prepare", "run simulator", "compare engines".
std::string command_path(const ParsedArgs& args);

using Handler = int (*)(const ParsedArgs&);

// Runs the command named by `args`.
int dispatch(const ParsedArgs& args);

// For tests: the handler registered for `path`, or null.
Handler find_handler(const std::string& path);

}  // namespace cli
}  // namespace dc_determinism
