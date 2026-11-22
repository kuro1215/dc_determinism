#include "common/identity.hpp"

#include <mutex>

#include "common/fs.hpp"

namespace dc_determinism {

std::string source_tree_sha256() { return kSourceTreeSha256; }

std::string binary_sha256() {
    static std::once_flag once;
    static std::string digest;
    std::call_once(once, [] { digest = sha256_file(executable_path()); });
    return digest;
}

std::string version_line() { return "dc_determinism " + source_tree_sha256(); }

}  // namespace dc_determinism
