// A scratch directory at one fixed absolute path, for builds whose bytes must not depend on where the repository is
// checked out (a compiler may embed the source path: MSVC derives the names of anonymous-namespace types from it).
// The directory is emptied when it is taken and removed when it is released; a second holder of the same path is
// refused (an exclusive lock on `<path>.lock`, which the operating system drops when the process dies).
#pragma once

#include <string>

#include "common/platform.hpp"

namespace dc_determinism {

// `<system drive>\dc_determinism-pb\b` on Windows, `/tmp/dc_determinism-pb/b` elsewhere; the environment variable
// DC_DETERMINISM_PREBUILD_SCRATCH replaces it (experiments and tests: another path may give other bytes).
fs::path default_scratch_path();

class ScratchDirectory {
public:
    // Takes the lock (DcDeterminismError when another run holds it), removes whatever is at `path` and creates it empty.
    explicit ScratchDirectory(const fs::path& path);
    ~ScratchDirectory();
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    void* lock_ = nullptr;  // the platform's lock handle
};

}  // namespace dc_determinism
