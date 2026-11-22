#include "common/fs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <system_error>

#include "common/error.hpp"
#include "common/sha256.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dc_determinism {

namespace {

std::FILE* open_file(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const char* c = mode; *c != '\0'; ++c) wide_mode.push_back(static_cast<wchar_t>(*c));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void flush_to_disk(std::FILE* file) {
    std::fflush(file);
#ifdef _WIN32
    _commit(_fileno(file));
#else
    fsync(fileno(file));
#endif
}

std::string errno_text(int code) { return std::strerror(code); }

bool is_exists_error(const std::error_code& code) {
    return code == std::errc::file_exists;
}

}  // namespace

std::string read_file(const fs::path& path) {
    std::FILE* file = open_file(path, "rb");
    if (file == nullptr) {
        throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": " + errno_text(errno));
    }
    std::string out;
    char buffer[1 << 16];
    for (;;) {
        std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
        out.append(buffer, count);
        if (count < sizeof(buffer)) break;
    }
    bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": read error");
    return out;
}

std::string sha256_file(const fs::path& path) {
    std::FILE* file = open_file(path, "rb");
    if (file == nullptr) {
        throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": " + errno_text(errno));
    }
    Sha256 hash;
    std::string buffer(1 << 20, '\0');
    for (;;) {
        std::size_t count = std::fread(&buffer[0], 1, buffer.size(), file);
        hash.update(buffer.data(), count);
        if (count < buffer.size()) break;
    }
    bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": read error");
    return hash.finish_hex();
}

std::string normalize_lf(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] == '\r' && i + 1 < bytes.size() && bytes[i + 1] == '\n') continue;
        out.push_back(bytes[i]);
    }
    return out;
}

std::string sha256_lf_file(const fs::path& path) { return sha256_hex(normalize_lf(read_file(path))); }

std::optional<fs::path> find_repo_root(const fs::path& start) {
    std::error_code ec;
    fs::path current = start;
    for (;;) {
        if (fs::is_regular_file(current / "prebuilt" / "manifest.json", ec) &&
            fs::is_directory(current / "src" / "vendor", ec)) {
            return current;
        }
        fs::path parent = current.parent_path();
        if (parent == current || parent.empty()) return std::nullopt;
        current = parent;
    }
}

fs::path repo_root() {
    static fs::path cached;
    if (!cached.empty()) return cached;
    std::optional<std::string> override_path = env_get("DC_DETERMINISM_REPO");
    if (override_path && !override_path->empty()) {
        cached = resolve(path_from_utf8(*override_path));
        return cached;
    }
    std::optional<fs::path> found = find_repo_root(executable_path().parent_path());
    if (!found) {
        throw DcDeterminismError("cannot locate the repository root (no directory above " +
                         path_to_utf8(executable_path()) +
                         " holds prebuilt/manifest.json and src/vendor/); set DC_DETERMINISM_REPO");
    }
    cached = *found;
    return cached;
}

fs::path default_logs_dir() { return repo_root() / "logs"; }

fs::path resolve(const fs::path& path) {
    std::error_code ec;
    fs::path absolute = fs::absolute(path, ec);
    if (ec) absolute = path;
    fs::path canonical = fs::weakly_canonical(absolute, ec);
    return ec ? absolute.lexically_normal() : canonical;
}

void refuse_existing(const fs::path& path) {
    std::error_code ec;
    if (fs::exists(fs::symlink_status(path, ec))) {
        throw DcDeterminismError("refusing to overwrite existing output: " + path_to_utf8(path));
    }
}

fs::path create_temporary_sibling(const fs::path& final_path) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
    fs::path directory = final_path.parent_path();
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string token;
        for (int i = 0; i < 8; ++i) token.push_back(alphabet[engine() % (sizeof(alphabet) - 1)]);
        fs::path candidate =
            directory / path_from_utf8(path_to_utf8(final_path.filename()) + "." + token + ".tmp");
        std::FILE* file = open_file(candidate, "wbx");
        if (file != nullptr) {
            std::fclose(file);
            return candidate;
        }
        if (errno != EEXIST) {
            throw DcDeterminismError("cannot create a temporary file next to " + path_to_utf8(final_path) +
                             ": " + errno_text(errno));
        }
    }
    throw DcDeterminismError("cannot create a temporary file next to " + path_to_utf8(final_path));
}

void commit_new(const fs::path& temporary, const fs::path& final_path) {
    std::error_code ec;
    fs::create_hard_link(temporary, final_path, ec);
    if (!ec) {
        fs::remove(temporary, ec);
        return;
    }
    if (is_exists_error(ec)) {
        throw DcDeterminismError("refusing to overwrite existing output: " + path_to_utf8(final_path));
    }
    // Hard links unsupported on this file system: reserve the destination exclusively, then
    // replace the placeholder that we own.
    std::FILE* reserved = open_file(final_path, "wbx");
    if (reserved == nullptr) {
        int code = errno;
        if (code == EEXIST) {
            fs::remove(temporary, ec);
            throw DcDeterminismError("refusing to overwrite existing output: " + path_to_utf8(final_path));
        }
        throw DcDeterminismError("cannot create " + path_to_utf8(final_path) + ": " + errno_text(code));
    }
    std::fclose(reserved);
    fs::rename(temporary, final_path, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(final_path, ignored);
        throw DcDeterminismError("cannot create " + path_to_utf8(final_path) + ": " + ec.message());
    }
}

void write_file_new(const fs::path& path, const std::string& bytes) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    refuse_existing(path);
    fs::path temporary = create_temporary_sibling(path);
    try {
        std::FILE* file = open_file(temporary, "wb");
        if (file == nullptr) {
            throw DcDeterminismError("cannot write " + path_to_utf8(temporary) + ": " + errno_text(errno));
        }
        std::size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
        flush_to_disk(file);
        bool failed = written != bytes.size() || std::ferror(file) != 0;
        std::fclose(file);
        if (failed) throw DcDeterminismError("cannot write " + path_to_utf8(temporary) + ": write error");
        commit_new(temporary, path);
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(temporary, ignored);
}

}  // namespace dc_determinism
