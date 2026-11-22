// Files, repository paths and atomic never-overwrite writes.
#pragma once

#include <string>

#include "common/platform.hpp"

namespace dc_determinism {

// Reads a whole file; failure: DcDeterminismError("cannot read <path>: <reason>").
std::string read_file(const fs::path& path);

std::string sha256_file(const fs::path& path);
// SHA-256 of a text file with CRLF normalised to LF (as stored in Git).
std::string sha256_lf_file(const fs::path& path);
std::string normalize_lf(const std::string& bytes);

// Repository root: DC_DETERMINISM_REPO, else the first directory above the executable that holds both
// `prebuilt/manifest.json` and `src/vendor/`. Throws DcDeterminismError when neither is found.
fs::path repo_root();
fs::path default_logs_dir();
// Pure search used by repo_root() (exposed for tests): walks up from `start`.
std::optional<fs::path> find_repo_root(const fs::path& start);

// Moves `temporary` to `final` without ever replacing an existing file
// (error: "refusing to overwrite existing output: <final>").
void commit_new(const fs::path& temporary, const fs::path& final_path);
// Throws the same error when `path` exists.
void refuse_existing(const fs::path& path);

// Writes `bytes` to a new file atomically: temp file in the same directory, fsync, commit_new.
// Creates missing parent directories; never overwrites.
void write_file_new(const fs::path& path, const std::string& bytes);

// Creates a fresh file `<name>.<random>.tmp` next to `final_path` and returns its path.
fs::path create_temporary_sibling(const fs::path& final_path);

// Python `Path.resolve()` for an existing or non-existing path: absolute, normalised.
fs::path resolve(const fs::path& path);

}  // namespace dc_determinism
