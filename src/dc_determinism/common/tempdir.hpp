// `tempfile.TemporaryDirectory(prefix=...)`: a unique scratch directory removed with its content.
#pragma once

#include <string>

#include "common/platform.hpp"

namespace dc_determinism {

class TempDir {
public:
    explicit TempDir(const std::string& prefix);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

}  // namespace dc_determinism
