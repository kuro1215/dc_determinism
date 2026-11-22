#include "common/scratch.hpp"

#include <cstdint>
#include <fstream>
#include <system_error>

#include "common/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace dc_determinism {

namespace {

const char* const kMarkerName = ".dc_determinism-scratch";

fs::path marker_of(const fs::path& path) { return path / kMarkerName; }

fs::path lock_path_of(const fs::path& path) {
    fs::path lock = path;
    lock += ".lock";
    return lock;
}

#ifdef _WIN32

// An open handle without sharing: no other process (or handle of this one) can open the file meanwhile. The file is
// deleted when the handle closes, so a crashed run leaves nothing behind.
void* acquire_lock(const fs::path& lock) {
    HANDLE handle = CreateFileW(lock.wstring().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

void release_lock(void* handle, const fs::path&) {
    if (handle != nullptr) CloseHandle(static_cast<HANDLE>(handle));
}

#else

void* acquire_lock(const fs::path& lock) {
    int descriptor = open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (descriptor < 0) return nullptr;
    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        close(descriptor);
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor) + 1);
}

void release_lock(void* handle, const fs::path& lock) {
    if (handle == nullptr) return;
    int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
    std::error_code ec;
    fs::remove(lock, ec);
    close(descriptor);
}

#endif

}  // namespace

fs::path default_scratch_path() {
    std::optional<std::string> given = env_get("DC_DETERMINISM_PREBUILD_SCRATCH");
    if (given && !given->empty()) return path_from_utf8(*given);
#ifdef _WIN32
    std::optional<std::string> drive = env_get("SystemDrive");
    return path_from_utf8((drive && !drive->empty() ? *drive : std::string("C:")) + "\\dc_determinism-pb\\b");
#else
    return fs::path("/tmp/dc_determinism-pb/b");
#endif
}

ScratchDirectory::ScratchDirectory(const fs::path& path) : path_(path) {
    if (path_.empty() || !path_.is_absolute()) {
        throw DcDeterminismError("the scratch path must be absolute: " + path_to_utf8(path_));
    }
    if (path_.root_path() == path_ || !path_.has_relative_path()) {
        throw DcDeterminismError("the scratch path must not be a drive or file system root: " + path_to_utf8(path_));
    }
    // The lock file sits next to the directory, in a parent that a user may create (a file in the root of the system
    // drive would need administrator rights).
    std::error_code ec;
    fs::create_directories(path_.parent_path(), ec);
    fs::path lock = lock_path_of(path_);
    lock_ = acquire_lock(lock);
    if (lock_ == nullptr) {
        throw DcDeterminismError("another run is using the scratch directory " + path_to_utf8(path_) + " (cannot lock " +
                         path_to_utf8(lock) + "); runs of `dc_determinism prebuild` do not overlap");
    }
    // Only a directory this tool made (it holds the marker) or an empty one is emptied; anything else is somebody else's data.
    if (fs::exists(path_, ec) && !fs::is_empty(path_, ec) && !fs::exists(marker_of(path_), ec)) {
        release_lock(lock_, lock);
        lock_ = nullptr;
        throw DcDeterminismError("refusing to use " + path_to_utf8(path_) + " as the scratch directory: it is not empty and was not " +
                         "created by this tool (no " + kMarkerName + " file); nothing was deleted");
    }
    fs::remove_all(path_, ec);
    if (ec) {
        release_lock(lock_, lock);
        lock_ = nullptr;
        throw DcDeterminismError("cannot remove " + path_to_utf8(path_) + ": " + ec.message());
    }
    fs::create_directories(path_, ec);
    if (!ec) {
        std::ofstream marker(marker_of(path_), std::ios::binary);
        if (!marker) ec = std::make_error_code(std::errc::io_error);
    }
    if (ec) {
        release_lock(lock_, lock);
        lock_ = nullptr;
        throw DcDeterminismError("cannot create " + path_to_utf8(path_) + ": " + ec.message());
    }
}

ScratchDirectory::~ScratchDirectory() {
    std::error_code ec;
    if (lock_ != nullptr && fs::exists(marker_of(path_), ec)) fs::remove_all(path_, ec);
    release_lock(lock_, lock_path_of(path_));
}

}  // namespace dc_determinism
