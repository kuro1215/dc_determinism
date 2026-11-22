#include "common/platform.hpp"

#include <cctype>
#include <cstdlib>
#include <system_error>
#include <thread>

#include "common/error.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace dc_determinism {

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) throw DcDeterminismError("invalid UTF-8 text");
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                   nullptr, nullptr);
    if (size <= 0) throw DcDeterminismError("invalid UTF-16 text");
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], size, nullptr,
                        nullptr);
    return out;
}

fs::path path_from_utf8(const std::string& text) { return fs::path(utf8_to_wide(text)); }

std::string path_to_utf8(const fs::path& path) { return wide_to_utf8(path.native()); }

std::vector<std::string> utf8_arguments(int, char**) {
    int count = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr) throw DcDeterminismError("cannot read the command line");
    std::vector<std::string> out;
    for (int i = 0; i < count; ++i) out.push_back(wide_to_utf8(wide[i]));
    LocalFree(wide);
    return out;
}

std::optional<std::string> env_get(const std::string& name) {
    std::wstring wide_name = utf8_to_wide(name);
    DWORD size = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (size == 0) {
        if (GetLastError() == ERROR_ENVVAR_NOT_FOUND) return std::nullopt;
        return std::string();
    }
    std::wstring value(size, L'\0');
    DWORD written = GetEnvironmentVariableW(wide_name.c_str(), &value[0], size);
    value.resize(written);
    return wide_to_utf8(value);
}

fs::path executable_path() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
        if (size == 0) throw DcDeterminismError("cannot determine the executable path");
        if (size < buffer.size()) {
            buffer.resize(size);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}

#else

fs::path path_from_utf8(const std::string& text) { return fs::path(text); }

std::string path_to_utf8(const fs::path& path) { return path.string(); }

std::vector<std::string> utf8_arguments(int argc, char** argv) {
    std::vector<std::string> out;
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    return out;
}

std::optional<std::string> env_get(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
}

fs::path executable_path() {
    std::vector<char> buffer(4096);
    for (;;) {
        ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (size < 0) throw DcDeterminismError("cannot determine the executable path");
        if (static_cast<std::size_t>(size) < buffer.size()) {
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(size)));
        }
        buffer.resize(buffer.size() * 2);
    }
}

#endif

std::optional<fs::path> find_on_path(const std::string& name) {
    if (name.empty()) return std::nullopt;
    std::optional<std::string> path_variable = env_get("PATH");
    if (!path_variable) return std::nullopt;
#ifdef _WIN32
    const char separator = ';';
    std::vector<std::string> extensions;
    {
        std::string pathext = env_get("PATHEXT").value_or(".COM;.EXE;.BAT;.CMD");
        std::size_t start = 0;
        while (start <= pathext.size()) {
            std::size_t end = pathext.find(';', start);
            if (end == std::string::npos) end = pathext.size();
            if (end > start) extensions.push_back(pathext.substr(start, end - start));
            start = end + 1;
        }
    }
    auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    // A name that already ends with a PATHEXT extension is tried as given first.
    std::vector<std::string> candidates;
    for (const std::string& extension : extensions) {
        std::string suffix = lower(extension);
        std::string lowered = lower(name);
        if (lowered.size() >= suffix.size() && lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
            candidates.push_back(name);
            break;
        }
    }
    for (const std::string& extension : extensions) candidates.push_back(name + extension);
#else
    const char separator = ':';
    std::vector<std::string> candidates{name};
#endif
    std::size_t start = 0;
    while (start <= path_variable->size()) {
        std::size_t end = path_variable->find(separator, start);
        if (end == std::string::npos) end = path_variable->size();
        std::string directory = path_variable->substr(start, end - start);
        start = end + 1;
        if (directory.empty()) continue;
        for (const std::string& candidate : candidates) {
            std::error_code ec;
            fs::path file = path_from_utf8(directory) / path_from_utf8(candidate);
            if (!fs::is_regular_file(file, ec)) continue;
#ifndef _WIN32
            if (access(file.c_str(), X_OK) != 0) continue;
#endif
            return file;
        }
    }
    return std::nullopt;
}

int cpu_count() {
#ifdef _WIN32
    DWORD active = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active > 0) return static_cast<int>(active);
#endif
    unsigned count = std::thread::hardware_concurrency();
    return count == 0 ? 1 : static_cast<int>(count);
}

}  // namespace dc_determinism
