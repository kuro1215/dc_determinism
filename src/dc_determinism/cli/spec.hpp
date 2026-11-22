// The dc_determinism command line: every command, option, default, bound, choice and mutual-exclusion rule
#pragma once

#include <memory>

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

std::unique_ptr<Parser> build_parser();

}  // namespace cli
}  // namespace dc_determinism
