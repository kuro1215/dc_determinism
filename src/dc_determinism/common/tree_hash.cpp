#include "common/tree_hash.hpp"

#include <algorithm>
#include <system_error>
#include <utility>
#include <vector>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/sha256.hpp"

namespace dc_determinism {

std::string tree_sha256_lf(const fs::path& directory) {
    std::error_code ec;
    // (POSIX relative path, absolute path) of every regular file.
    std::vector<std::pair<std::string, fs::path>> files;
    fs::recursive_directory_iterator it(directory, fs::directory_options::none, ec);
    if (ec) throw DcDeterminismError("cannot read " + path_to_utf8(directory) + ": " + ec.message());
    for (const fs::recursive_directory_iterator end; it != end; it.increment(ec)) {
        if (ec) throw DcDeterminismError("cannot read " + path_to_utf8(directory) + ": " + ec.message());
        const fs::directory_entry& entry = *it;
        fs::file_status link_status = entry.symlink_status(ec);
        if (fs::is_symlink(link_status)) {
            throw DcDeterminismError("vendored tree contains a symlink: " + path_to_utf8(entry.path()));
        }
        if (!fs::is_regular_file(link_status)) continue;
        std::string relative = path_to_utf8(entry.path().lexically_relative(directory));
#ifdef _WIN32
        std::replace(relative.begin(), relative.end(), '\\', '/');
#endif
        files.emplace_back(relative, entry.path());
    }
    std::sort(files.begin(), files.end(),
              [](const std::pair<std::string, fs::path>& a, const std::pair<std::string, fs::path>& b) {
                  return a.first < b.first;
              });
    Sha256 digest;
    for (const std::pair<std::string, fs::path>& item : files) {
        std::array<std::uint8_t, 32> content = [&item] {
            Sha256 file_hash;
            file_hash.update(normalize_lf(read_file(item.second)));
            return file_hash.finish_digest();
        }();
        digest.update(item.first);
        digest.update("\0", 1);
        digest.update(content.data(), content.size());
    }
    return digest.finish_hex();
}

}  // namespace dc_determinism
