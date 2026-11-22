// Tree hash: sorted
// POSIX paths, each contributing `<path>\0<sha256(LF-normalised bytes)>` (raw 32-byte digest).
#pragma once

#include <string>

#include "common/platform.hpp"

namespace dc_determinism {

// A symlink anywhere below `directory` is an error ("vendored tree contains a symlink: <path>").
std::string tree_sha256_lf(const fs::path& directory);

}  // namespace dc_determinism
