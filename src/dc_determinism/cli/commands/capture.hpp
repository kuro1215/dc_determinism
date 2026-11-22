// `capture simulator` (the C API engines through the worker, the pybind engines through the v2 recording
// binaries) and `compare engines`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "json/json.hpp"

namespace dc_determinism {
namespace cli {

int command_capture_simulator(const ParsedArgs& args);
int command_compare_engines(const ParsedArgs& args);

// The prepared positions (ascending, without duplicates) of plain shot ids; DcDeterminismError when an id is not in the
// prepared corpus (`<source> lists N shots that are not in the prepared corpus, e.g. '<id>'`).
std::vector<std::int64_t> positions_of_ids(const std::vector<std::string>& ids, const Json& shots,
                                           const std::string& source);

}  // namespace cli
}  // namespace dc_determinism
