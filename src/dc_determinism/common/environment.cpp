#include "common/environment.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

#include "common/platform.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif
#endif

namespace dc_determinism {
namespace environment {

namespace {

std::string trim(const std::string& text) {
    std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    std::size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

#ifdef _WIN32

struct WindowsVersion {
    DWORD major = 0, minor = 0, build = 0;
};

WindowsVersion windows_version() {
    WindowsVersion version;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    RtlGetVersionFn function =
        ntdll ? reinterpret_cast<RtlGetVersionFn>(reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
              : nullptr;
    if (function != nullptr) {
        OSVERSIONINFOW info = {};
        info.dwOSVersionInfoSize = sizeof(info);
        if (function(&info) == 0) {
            version.major = info.dwMajorVersion;
            version.minor = info.dwMinorVersion;
            version.build = info.dwBuildNumber;
        }
    }
    return version;
}

std::string machine_name() {
    SYSTEM_INFO info;
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64: return "AMD64";
        case PROCESSOR_ARCHITECTURE_ARM64: return "ARM64";
        case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
        default: return std::string();
    }
}

std::optional<std::string> cpu_model() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ,
                      &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    wchar_t buffer[512];
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    DWORD type = 0;
    LONG status = RegQueryValueExW(key, L"ProcessorNameString", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) return std::nullopt;
    buffer[size / sizeof(wchar_t)] = L'\0';
    return trim(wide_to_utf8(buffer));
}

#else

std::optional<std::string> cpu_model() {
    std::ifstream stream("/proc/cpuinfo");
    std::stringstream buffer;
    buffer << stream.rdbuf();
    return cpu_model_from_cpuinfo(buffer.str());
}

#endif

}  // namespace

std::optional<std::string> cpu_model_from_cpuinfo(const std::string& text) {
    std::optional<std::string> implementer;
    std::optional<std::string> part;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        std::size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string value = trim(line.substr(colon + 1));
        if (key == "model name") return value;
        if (key == "cpu implementer" && !implementer) implementer = value;
        if (key == "cpu part" && !part) part = value;
    }
    // aarch64 kernels (Apple Silicon, emulated CPUs) have no "model name": describe the first core by its ids.
    if (!implementer || !part) return std::nullopt;
    for (std::string* item : {&*implementer, &*part}) {
        for (char& c : *item) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string vendor;
    switch (std::strtoul(implementer->c_str(), nullptr, 0)) {
        case 0x61: vendor = "Apple "; break;
        case 0x41: vendor = "Arm "; break;
        case 0x00: vendor = "Emulated/unknown "; break;
        default: break;
    }
    return vendor + "CPU implementer " + *implementer + " part " + *part;
}

bool in_container() {
    std::error_code ec;
    return fs::exists("/.dockerenv", ec) || fs::exists("/run/.containerenv", ec);
}

Json collect() {
    Json out = Json::object();
#ifdef _WIN32
    WindowsVersion version = windows_version();
    // platform.release(): "11" from build 22000 on, else the major version ("10").
    std::string release = version.major == 10 && version.build >= 22000 ? "11" : std::to_string(version.major);
    out["os"] = "Windows";
    out["os_version"] = std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
                        std::to_string(version.build);
    out["kernel"] = release;
    out["machine"] = machine_name();
#else
    struct utsname name;
    uname(&name);
    std::string system = name.sysname;
    std::string platform_text = system + "-" + name.release + "-" + name.machine;
#ifdef __GLIBC__
    platform_text += std::string("-with-glibc") + gnu_get_libc_version();
#endif
    out["os"] = system;
    out["os_version"] = platform_text;
    out["kernel"] = std::string(name.release);
    out["machine"] = std::string(name.machine);
#endif
    out["python"] = nullptr;
    std::optional<std::string> cpu = cpu_model();
    out["cpu_model"] = cpu ? Json(*cpu) : Json(nullptr);
    out["logical_cpus"] = cpu_count();
    out["container"] = in_container();
    out["implementation"] = "dc_determinism";
    return out;
}

}  // namespace environment
}  // namespace dc_determinism
