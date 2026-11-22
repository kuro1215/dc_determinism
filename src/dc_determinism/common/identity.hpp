// Tool identity: this tool records
//  * the tree hash of its own sources (src/dc_determinism, LF-normalised, same definition as
//    `tree_sha256_lf`), computed at build time and embedded in the binary, and
//  * the SHA-256 of the running binary.
#pragma once

#include <string>

namespace dc_determinism {

// Defined in the generated identity source (cmake: dc_determinism_treehash --emit-cpp).
extern const char* const kSourceTreeSha256;

std::string source_tree_sha256();
// SHA-256 of the running executable (computed once, on first use).
std::string binary_sha256();
// `dc_determinism <source tree sha256>` (what `--version` prints).
std::string version_line();

}  // namespace dc_determinism
