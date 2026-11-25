// `render trajectory`: SVG plots of captured shots with the collisions inferred from the stone positions.
#pragma once

#include "cli/argparse.hpp"

namespace dc_determinism {
namespace cli {

int command_render_trajectory(const ParsedArgs& args);

}  // namespace cli
}  // namespace dc_determinism
