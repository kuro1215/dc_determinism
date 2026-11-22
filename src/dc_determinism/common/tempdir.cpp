#include "common/tempdir.hpp"

#include <random>
#include <system_error>

#include "common/error.hpp"

namespace dc_determinism {

TempDir::TempDir(const std::string& prefix) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec) throw DcDeterminismError("cannot find a temporary directory: " + ec.message());
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string token;
        for (int i = 0; i < 8; ++i) token.push_back(alphabet[engine() % (sizeof(alphabet) - 1)]);
        fs::path candidate = base / path_from_utf8(prefix + token);
        if (fs::create_directory(candidate, ec)) {
            path_ = candidate;
            return;
        }
    }
    throw DcDeterminismError("cannot create a temporary directory below " + path_to_utf8(base));
}

TempDir::~TempDir() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
}

}  // namespace dc_determinism
