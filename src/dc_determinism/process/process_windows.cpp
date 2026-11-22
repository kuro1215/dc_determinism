// Windows half of the process layer: CreateProcessW with wide arguments, pipes limited to an
// explicit inheritable-handle list, and a kill-on-close job object per child.
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "common/error.hpp"
#include "common/platform.hpp"
#include "process/process_native.hpp"

namespace dc_determinism {
namespace native {

struct Child {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stderr_read = nullptr;
};

namespace {

void close_handle(HANDLE& handle) {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

std::string last_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::string text;
    if (size != 0 && buffer != nullptr) {
        std::wstring wide(buffer, size);
        while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L' ')) {
            wide.pop_back();
        }
        text = wide_to_utf8(wide);
    }
    if (buffer != nullptr) LocalFree(buffer);
    return text.empty() ? "error " + std::to_string(code) : text;
}

// Quotes one argument for CommandLineToArgvW / the MSVC runtime.
void append_quoted(std::wstring& out, const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += argument;
        return;
    }
    out.push_back(L'"');
    std::size_t backslashes = 0;
    for (wchar_t c : argument) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
}

std::wstring upper(const std::wstring& text) {
    std::wstring out = text;
    CharUpperBuffW(&out[0], static_cast<DWORD>(out.size()));
    return out;
}

// The environment block (sorted, double-NUL terminated) with `changes` applied.
std::wstring build_environment_block(const StartOptions& options) {
    std::vector<std::wstring> entries;
    LPWCH block = GetEnvironmentStringsW();
    if (block != nullptr) {
        for (const wchar_t* item = block; *item != L'\0'; item += std::wcslen(item) + 1) {
            entries.emplace_back(item);
        }
        FreeEnvironmentStringsW(block);
    }
    for (const std::pair<std::string, std::string>& change : options.environment) {
        std::wstring name = upper(utf8_to_wide(change.first));
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& entry) {
                                         // The first character may be '=' (drive variables).
                                         std::size_t equals = entry.find(L'=', 1);
                                         return equals != std::wstring::npos &&
                                                upper(entry.substr(0, equals)) == name;
                                     }),
                      entries.end());
        if (!change.second.empty()) entries.push_back(utf8_to_wide(change.first + "=" + change.second));
    }
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring out;
    for (const std::wstring& entry : entries) {
        out += entry;
        out.push_back(L'\0');
    }
    out.push_back(L'\0');
    return out;
}

HANDLE open_nul(bool for_reading) {
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    return CreateFileW(L"NUL", for_reading ? GENERIC_READ : GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

// An inheritable handle for the child (a duplicate of `source`, or NUL when there is none).
HANDLE inheritable_copy(HANDLE source, bool for_reading) {
    if (source == nullptr || source == INVALID_HANDLE_VALUE) return open_nul(for_reading);
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return open_nul(for_reading);
    }
    return copy;
}

// Creates a pipe; `child_end` is inheritable, `parent_end` is not.
bool make_pipe(HANDLE* parent_end, HANDLE* child_end, bool parent_reads) {
    SECURITY_ATTRIBUTES security = {sizeof(security), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &security, 1 << 16)) return false;
    HANDLE parent = parent_reads ? read_end : write_end;
    SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0);
    *parent_end = parent;
    *child_end = parent_reads ? write_end : read_end;
    return true;
}

}  // namespace

Child* spawn(const StartOptions& options, std::int64_t* pid) {
    std::unique_ptr<Child> child(new Child);
    const std::string& program = options.argv[0];

    HANDLE child_in = nullptr;
    HANDLE child_out = nullptr;
    HANDLE child_err = nullptr;
    auto cleanup = [&] {
        close_handle(child_in);
        close_handle(child_out);
        close_handle(child_err);
        close_handle(child->stdin_write);
        close_handle(child->stdout_read);
        close_handle(child->stderr_read);
        close_handle(child->job);
    };

    bool ok = true;
    if (options.pipe_stdin) {
        ok = ok && make_pipe(&child->stdin_write, &child_in, false);
    } else {
        child_in = open_nul(true);
        ok = ok && child_in != INVALID_HANDLE_VALUE;
    }
    if (options.pipe_stdout) {
        ok = ok && make_pipe(&child->stdout_read, &child_out, true);
    } else {
        child_out = inheritable_copy(GetStdHandle(STD_OUTPUT_HANDLE), false);
    }
    switch (options.error_stream) {
        case ErrorStream::Pipe: ok = ok && make_pipe(&child->stderr_read, &child_err, true); break;
        case ErrorStream::Null: child_err = open_nul(false); break;
        case ErrorStream::Stdout: child_err = inheritable_copy(child_out, false); break;
        case ErrorStream::Inherit: child_err = inheritable_copy(GetStdHandle(STD_ERROR_HANDLE), false); break;
    }
    if (!ok || child_in == INVALID_HANDLE_VALUE || child_out == INVALID_HANDLE_VALUE ||
        child_err == INVALID_HANDLE_VALUE) {
        DWORD code = GetLastError();
        cleanup();
        throw DcDeterminismError("cannot start " + program + ": " + last_error_text(code));
    }

    // Only the three std handles are inherited (a list of unique handles).
    std::vector<HANDLE> inherited = {child_in, child_out, child_err};
    std::sort(inherited.begin(), inherited.end());
    inherited.erase(std::unique(inherited.begin(), inherited.end()), inherited.end());
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<char> attribute_storage(attribute_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size);
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                              inherited.size() * sizeof(HANDLE), nullptr, nullptr);

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_in;
    startup.StartupInfo.hStdOutput = child_out;
    startup.StartupInfo.hStdError = child_err;
    startup.lpAttributeList = attributes;

    std::wstring command_line;
    for (std::size_t i = 0; i < options.argv.size(); ++i) {
        if (i > 0) command_line.push_back(L' ');
        append_quoted(command_line, utf8_to_wide(options.argv[i]));
    }
    std::wstring working_directory = utf8_to_wide(options.working_directory);
    std::wstring environment;
    bool custom_environment = !options.environment.empty();
    if (custom_environment) environment = build_environment_block(options);

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW;
    if (custom_environment) flags |= CREATE_UNICODE_ENVIRONMENT;
    PROCESS_INFORMATION info = {};
    BOOL created = CreateProcessW(nullptr, &command_line[0], nullptr, nullptr, TRUE, flags,
                                  custom_environment ? &environment[0] : nullptr,
                                  working_directory.empty() ? nullptr : working_directory.c_str(),
                                  &startup.StartupInfo, &info);
    DWORD create_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    close_handle(child_in);
    close_handle(child_out);
    close_handle(child_err);
    if (!created) {
        cleanup();
        throw DcDeterminismError("cannot start " + program + ": " + last_error_text(create_error));
    }
    CloseHandle(info.hThread);
    child->process = info.hProcess;

    child->job = CreateJobObjectW(nullptr, nullptr);
    if (child->job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(child->job, child->process)) close_handle(child->job);
    }
    *pid = static_cast<std::int64_t>(info.dwProcessId);
    return child.release();
}

namespace {

std::size_t read_handle(HANDLE handle, char* buffer, std::size_t size) {
    if (handle == nullptr) return 0;
    DWORD count = 0;
    DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
    if (!ReadFile(handle, buffer, wanted, &count, nullptr)) return 0;
    return count;
}

}  // namespace

std::size_t read_stdout(Child* child, char* buffer, std::size_t size) {
    return read_handle(child->stdout_read, buffer, size);
}

std::size_t read_stderr(Child* child, char* buffer, std::size_t size) {
    return read_handle(child->stderr_read, buffer, size);
}

bool write_stdin(Child* child, const char* data, std::size_t size) {
    HANDLE handle = child->stdin_write;
    while (size > 0) {
        if (handle == nullptr) return false;
        DWORD written = 0;
        DWORD wanted = static_cast<DWORD>(std::min<std::size_t>(size, 1u << 20));
        if (!WriteFile(handle, data, wanted, &written, nullptr)) return false;
        data += written;
        size -= written;
    }
    return true;
}

void close_stdin(Child* child) { close_handle(child->stdin_write); }

bool wait(Child* child, double timeout_seconds, std::int64_t* code) {
    DWORD milliseconds = INFINITE;
    if (timeout_seconds >= 0) {
        double scaled = std::ceil(timeout_seconds * 1000.0);
        milliseconds = scaled >= 4.0e9 ? 4000000000u : static_cast<DWORD>(scaled);
    }
    DWORD result = WaitForSingleObject(child->process, milliseconds);
    if (result != WAIT_OBJECT_0) return false;
    DWORD exit_code = 0;
    GetExitCodeProcess(child->process, &exit_code);
    *code = static_cast<std::int64_t>(exit_code);
    return true;
}

void kill(Child* child) {
    if (child->job != nullptr) {
        TerminateJobObject(child->job, 1);
    } else {
        TerminateProcess(child->process, 1);
    }
}

void destroy(Child* child) {
    close_handle(child->stdin_write);
    close_handle(child->stdout_read);
    close_handle(child->stderr_read);
    close_handle(child->job);
    close_handle(child->process);
    delete child;
}

}  // namespace native
}  // namespace dc_determinism

#endif  // _WIN32
