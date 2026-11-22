// `prepare`: freeze the shot corpus into a prepared directory (C++ only).
#pragma once

#include <functional>

#include "cli/argparse.hpp"
#include "json/json.hpp"

namespace dc_determinism {
namespace cli {

int command_prepare(const ParsedArgs& args);

// The tool identity recorded as `preparer.dc_determinism` (source tree hash, binary hash). main()
// installs it; the identity library is not linked into the unit tests, which get placeholders.
void set_tool_identity_provider(std::function<Json()> provider);
Json tool_identity();

}  // namespace cli
}  // namespace dc_determinism
