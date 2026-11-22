// Child processes: one small cross-platform layer (Windows CreateProcessW with wide arguments,
// POSIX posix_spawn). Used for the C API workers, players and build tools.
//
// Contract
//  * Arguments, paths and text are UTF-8 `std::string`.
//  * `Process::start` throws DcDeterminismError when the program cannot be started.
//  * Pipes are blocking; the owner decides which thread reads and writes. `kill()` and
//    `ProcessGroup::kill_all()` are safe to call from any thread, so a failure path can kill every
//    worker first (unblocking readers), and join its threads afterwards.
//  * A `Process` that is still running when destroyed is killed and reaped.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace dc_determinism {

enum class ErrorStream {
    Inherit,  // the child writes to this process's stderr
    Pipe,     // collected by a background thread; `Process::stderr_text()` returns it
    Null,     // discarded
    Stdout,   // merged into the stdout pipe (requires pipe_stdout)
};

struct StartOptions {
    std::vector<std::string> argv;  // argv[0]: program (path, or a name searched on PATH)
    std::string working_directory;  // empty: inherit
    // Environment changes applied on top of this process's environment; an empty value removes
    // the variable.
    std::vector<std::pair<std::string, std::string>> environment;
    bool pipe_stdin = false;   // false: the child gets an empty stdin (NUL)
    bool pipe_stdout = false;  // false: inherit this process's stdout
    ErrorStream error_stream = ErrorStream::Inherit;
};

class Process {
public:
    static std::unique_ptr<Process> start(const StartOptions& options);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // Writes to the child's stdin; false when the pipe is closed (child exited).
    bool write_stdin(const std::string& data);
    void close_stdin();

    // Reads one line from stdout without its "\n" (a trailing "\r" is kept); false at EOF.
    bool read_line(std::string& line);
    // Everything up to EOF.
    std::string read_all_stdout();
    // Collected stderr (ErrorStream::Pipe); complete once the process has been waited for.
    std::string stderr_text();

    // Waits at most `timeout_seconds` (< 0: forever); true when the process has exited.
    bool wait_for(double timeout_seconds);
    // Waits for the exit and returns the code (POSIX: -signal when killed by a signal).
    std::int64_t wait();
    bool running();
    // The exit code after the process has exited (wait()/wait_for() true).
    std::int64_t exit_code() const;
    void kill();

    std::int64_t pid() const;

private:
    Process();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A set of processes that can be killed together from any thread.
class ProcessGroup {
public:
    void add(Process* process);
    void remove(Process* process);
    void kill_all();

private:
    std::mutex mutex_;
    std::vector<Process*> processes_;
};

struct RunResult {
    std::int64_t exit_code = 0;
    bool timed_out = false;
    std::string out;
    std::string err;
};

// Runs a program to completion, feeding `input` to its stdin and collecting stdout and stderr
// (`subprocess.run(capture_output=True, input=..., timeout=...)`); a timeout kills the process.
// `timeout_seconds` <= 0: no limit.
RunResult run_capture(const StartOptions& options, const std::string& input, double timeout_seconds);

}  // namespace dc_determinism
