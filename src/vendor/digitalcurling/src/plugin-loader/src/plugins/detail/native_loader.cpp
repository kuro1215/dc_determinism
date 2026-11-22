#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #define NOMINMAX

    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include "digitalcurling/plugins/plugin_error.hpp"
#include "digitalcurling/plugins/detail/native_loader.hpp"
#include "utf8_path.hpp"

namespace digitalcurling::plugins::detail {

std::string GetLastErrorMessage() {

#ifdef _WIN32

    DWORD code = ::GetLastError();
    if (code == 0) return "Failed to load plugin. No error code available.";

    LPWSTR buffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
        code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (LPWSTR)&buffer, 0, NULL);

    if (size > 0) {
        auto message = [buffer]() -> std::optional<std::string> {
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, NULL, 0, NULL, NULL);
            if (size_needed > 0) {
                std::string message;
                message.resize(size_needed);
                if (WideCharToMultiByte(CP_UTF8, 0, buffer, -1, &message[0], size_needed, NULL, NULL) > 0) {
                    message.resize(size_needed - 1);
                    return message;
                }
            }
            return std::nullopt;
        }();
        LocalFree(buffer);

        if (message.has_value()) {
            std::string msg = message.value();
            while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) {
                msg.pop_back();
            }
            return msg;
        }
    }
    return "Failed to load plugin. Unable to retrieve detailed error information. (error code " + std::to_string(code) + ")";

#else
    auto error = dlerror();
    if (error) return std::string(error);
    else return "Failed to load plugin. Unable to retrieve detailed error information.";
#endif

}

NativePlugin LoadNativePlugin(const std::filesystem::path& path) {
    constexpr const char* get_plugin_api_v2_name = "dc_get_plugin_api_v2";

    std::optional<ModulePtr> ptr = std::nullopt;

#if defined(_WIN32)
    // Search the plugin's own directory and the default safe directories only (not the CWD or PATH).
    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR requires an absolute path.
    const std::filesystem::path absolute_path = path.is_absolute() ? path : std::filesystem::absolute(path);
    HMODULE handle = LoadLibraryExW(absolute_path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!handle) {
        const auto error = GetLastErrorMessage();
        throw plugin_error { DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN, "\"" + PathToUtf8(path) + "\": " + error };
    }
    ptr = ModulePtr(static_cast<void*>(handle), [](void* p) { FreeLibrary(static_cast<HMODULE>(p)); });
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const auto error = GetLastErrorMessage();
        throw plugin_error { DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN, "\"" + PathToUtf8(path) + "\": " +
            (IsValidUtf8(error) ? error : "Failed to load plugin. Error details are not valid UTF-8.") };
    }
    ptr = ModulePtr(handle, [](void* p) { dlclose(p); });
#endif

#ifdef _WIN32
    auto get_v2_func_ptr = GetProcAddress(handle, get_plugin_api_v2_name);
#else
    auto get_v2_func_ptr = dlsym(handle, get_plugin_api_v2_name);
#endif

    if (!get_v2_func_ptr) {
        throw plugin_error {
            DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN,
            std::string("Plugin is missing required entry point function: ") + get_plugin_api_v2_name
        };
    }

    return NativePlugin{
        std::move(ptr.value()),
        reinterpret_cast<DigitalCurling_GetPluginApiV2Fn>(get_v2_func_ptr)
    };
}

} // namespace digitalcurling::plugins::detail
