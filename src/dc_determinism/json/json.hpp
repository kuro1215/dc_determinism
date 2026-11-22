// The JSON value type: nlohmann/json (src/vendor/nlohmann-json) with insertion-ordered objects,
// so that output key order follows the order the code built the value in (like a Python dict).
#pragma once

#include <nlohmann/json.hpp>

namespace dc_determinism {

using Json = nlohmann::ordered_json;

}  // namespace dc_determinism
