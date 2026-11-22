// Diagnostics go to stderr, one flushed line per message.
#pragma once

#include <string>

namespace dc_determinism {

void log(const std::string& message);

}  // namespace dc_determinism
