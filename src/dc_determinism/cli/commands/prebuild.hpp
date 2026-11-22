// `prebuild`: rebuilds the prebuilt dc_worker and player binaries and prebuilt/manifest.json
// (prebuild/prebuild.hpp).
#pragma once

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

int command_prebuild(const ParsedArgs& args);

}  // namespace cli
}  // namespace dc_determinism
