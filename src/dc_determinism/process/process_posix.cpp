// POSIX half of the process layer: posix_spawn with pipes.
#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/error.hpp"
#include "process/process_native.hpp"

extern char** environ;

namespace dc_determinism {
namespace native {

struct Child {
    pid_t pid = -1;
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    std::mutex mutex;  // serialises waitpid and kill so a recycled pid is never signalled
    bool reaped = false;
    std::int64_t code = 0;
};

namespace {

void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

bool make_pipe(int fds[2]) {
#ifdef __linux__
    return ::pipe2(fds, O_CLOEXEC) == 0;
#else
    if (::pipe(fds) != 0) return false;
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    return true;
#endif
}

std::vector<std::string> build_environment(const StartOptions& options) {
    std::vector<std::string> entries;
    for (char** item = environ; item != nullptr && *item != nullptr; ++item) entries.emplace_back(*item);
    for (const std::pair<std::string, std::string>& change : options.environment) {
        std::string prefix = change.first + "=";
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::string& entry) {
                                         return entry.compare(0, prefix.size(), prefix) == 0;
                                     }),
                      entries.end());
        if (!change.second.empty()) entries.push_back(prefix + change.second);
    }
    return entries;
}

}  // namespace

Child* spawn(const StartOptions& options, std::int64_t* pid) {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });

    std::unique_ptr<Child> child(new Child);
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    auto cleanup = [&] {
        for (int* pair : {in_pipe, out_pipe, err_pipe}) {
            close_fd(pair[0]);
            close_fd(pair[1]);
        }
    };
    const std::string& program = options.argv[0];
    if ((options.pipe_stdin && !make_pipe(in_pipe)) || (options.pipe_stdout && !make_pipe(out_pipe)) ||
        (options.error_stream == ErrorStream::Pipe && !make_pipe(err_pipe))) {
        int code = errno;
        cleanup();
        throw DcDeterminismError("cannot start " + program + ": " + std::strerror(code));
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (options.pipe_stdin) {
        posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    } else {
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    }
    if (options.pipe_stdout) posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    switch (options.error_stream) {
        case ErrorStream::Pipe: posix_spawn_file_actions_adddup2(&actions, err_pipe[1], 2); break;
        case ErrorStream::Null: posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0); break;
        case ErrorStream::Stdout: posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 2); break;
        case ErrorStream::Inherit: break;
    }
    if (!options.working_directory.empty()) {
        posix_spawn_file_actions_addchdir_np(&actions, options.working_directory.c_str());
    }

    std::vector<std::string> arguments = options.argv;
    std::vector<char*> argv;
    for (std::string& item : arguments) argv.push_back(&item[0]);
    argv.push_back(nullptr);
    std::vector<std::string> environment = build_environment(options);
    std::vector<char*> envp;
    for (std::string& item : environment) envp.push_back(&item[0]);
    envp.push_back(nullptr);

    pid_t child_pid = -1;
    int status = program.find('/') != std::string::npos
                     ? posix_spawn(&child_pid, program.c_str(), &actions, nullptr, argv.data(), envp.data())
                     : posix_spawnp(&child_pid, program.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (status != 0) {
        cleanup();
        throw DcDeterminismError("cannot start " + program + ": " + std::strerror(status));
    }
    child->pid = child_pid;
    if (options.pipe_stdin) {
        child->stdin_fd = in_pipe[1];
        close_fd(in_pipe[0]);
    }
    if (options.pipe_stdout) {
        child->stdout_fd = out_pipe[0];
        close_fd(out_pipe[1]);
    }
    if (options.error_stream == ErrorStream::Pipe) {
        child->stderr_fd = err_pipe[0];
        close_fd(err_pipe[1]);
    }
    *pid = child_pid;
    return child.release();
}

namespace {

std::size_t read_fd(int fd, char* buffer, std::size_t size) {
    if (fd < 0) return 0;
    for (;;) {
        ssize_t count = ::read(fd, buffer, size);
        if (count >= 0) return static_cast<std::size_t>(count);
        if (errno != EINTR) return 0;
    }
}

}  // namespace

std::size_t read_stdout(Child* child, char* buffer, std::size_t size) {
    return read_fd(child->stdout_fd, buffer, size);
}

std::size_t read_stderr(Child* child, char* buffer, std::size_t size) {
    return read_fd(child->stderr_fd, buffer, size);
}

bool write_stdin(Child* child, const char* data, std::size_t size) {
    int fd = child->stdin_fd;
    while (size > 0) {
        if (fd < 0) return false;
        ssize_t count = ::write(fd, data, size);
        if (count < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += count;
        size -= static_cast<std::size_t>(count);
    }
    return true;
}

void close_stdin(Child* child) { close_fd(child->stdin_fd); }

bool wait(Child* child, double timeout_seconds, std::int64_t* code) {
    auto start = std::chrono::steady_clock::now();
    auto pause = std::chrono::microseconds(200);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(child->mutex);
            if (child->reaped) {
                *code = child->code;
                return true;
            }
            int status = 0;
            pid_t result = ::waitpid(child->pid, &status, WNOHANG);
            if (result == child->pid) {
                child->reaped = true;
                child->code = WIFEXITED(status) ? WEXITSTATUS(status)
                                                : WIFSIGNALED(status) ? -static_cast<int>(WTERMSIG(status)) : -1;
                *code = child->code;
                return true;
            }
            if (result < 0 && errno != EINTR) {
                child->reaped = true;
                child->code = -1;
                *code = -1;
                return true;
            }
        }
        if (timeout_seconds >= 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= timeout_seconds) {
            return false;
        }
        std::this_thread::sleep_for(pause);
        if (pause < std::chrono::milliseconds(5)) pause *= 2;
    }
}

void kill(Child* child) {
    std::lock_guard<std::mutex> lock(child->mutex);
    if (!child->reaped) ::kill(child->pid, SIGKILL);
}

void destroy(Child* child) {
    close_fd(child->stdin_fd);
    close_fd(child->stdout_fd);
    close_fd(child->stderr_fd);
    delete child;
}

}  // namespace native
}  // namespace dc_determinism

#endif  // !_WIN32
