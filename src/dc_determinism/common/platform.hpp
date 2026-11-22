// Host abstraction: UTF-8 strings everywhere inside the tool, converted at the OS boundary.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dc_determinism {

namespace fs = std::filesystem;

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);
#endif

// Paths are carried as UTF-8 `std::string`; these convert at the file-system boundary.
fs::path path_from_utf8(const std::string& text);
std::string path_to_utf8(const fs::path& path);

// Command-line arguments as UTF-8 (on Windows from the wide command line).
std::vector<std::string> utf8_arguments(int argc, char** argv);

std::optional<std::string> env_get(const std::string& name);

// Absolute path of the running executable.
fs::path executable_path();

// `shutil.which(name)` without the current directory: the first executable `name` on PATH (on
// Windows with the PATHEXT extensions tried in order); nullopt when there is none.
std::optional<fs::path> find_on_path(const std::string& name);

// `os.cpu_count() or 1`.
int cpu_count();

constexpr bool is_windows() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

}  // namespace dc_determinism
