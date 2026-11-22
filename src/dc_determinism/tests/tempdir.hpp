// A unique scratch directory, removed with its content when the object goes away.
#pragma once

#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

namespace testing {

class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::mt19937_64 engine((static_cast<unsigned long long>(device()) << 32) ^ device());
        std::filesystem::path base = std::filesystem::temp_directory_path();
        for (;;) {
            path_ = base / ("dc_determinism-test-" + std::to_string(engine() % 1000000000ULL));
            std::error_code ec;
            if (std::filesystem::create_directory(path_, ec)) break;
        }
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
};

}  // namespace testing
