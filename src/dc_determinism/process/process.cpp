#include "process/process.hpp"

#include <algorithm>
#include <thread>

#include "common/error.hpp"
#include "process/process_native.hpp"

namespace dc_determinism {

struct Process::Impl {
    native::Child* child = nullptr;
    std::int64_t pid = 0;
    bool stdin_open = false;
    bool pipe_stdout = false;
    std::string stdout_buffer;  // bytes read but not yet returned
    bool stdout_eof = false;
    std::thread stderr_thread;
    std::string stderr_data;
    std::mutex state_mutex;
    bool exited = false;
    std::int64_t code = 0;
};

Process::Process() : impl_(new Impl) {}

std::unique_ptr<Process> Process::start(const StartOptions& options) {
    if (options.argv.empty()) throw DcDeterminismError("cannot start a process without a program");
    if (options.error_stream == ErrorStream::Stdout && !options.pipe_stdout) {
        throw DcDeterminismError("stderr can only be merged into a piped stdout");
    }
    std::unique_ptr<Process> process(new Process());
    Impl& impl = *process->impl_;
    impl.child = native::spawn(options, &impl.pid);
    impl.stdin_open = options.pipe_stdin;
    impl.pipe_stdout = options.pipe_stdout;
    impl.stdout_eof = !options.pipe_stdout;
    if (options.error_stream == ErrorStream::Pipe) {
        Impl* shared = &impl;
        impl.stderr_thread = std::thread([shared] {
            char buffer[4096];
            for (;;) {
                std::size_t count = native::read_stderr(shared->child, buffer, sizeof(buffer));
                if (count == 0) break;
                shared->stderr_data.append(buffer, count);
            }
        });
    }
    return process;
}

Process::~Process() {
    if (impl_->child == nullptr) return;
    if (!running()) {
        // already reaped
    } else {
        kill();
        wait();
    }
    if (impl_->stderr_thread.joinable()) impl_->stderr_thread.join();
    native::destroy(impl_->child);
}

bool Process::write_stdin(const std::string& data) {
    if (!impl_->stdin_open) return false;
    return native::write_stdin(impl_->child, data.data(), data.size());
}

void Process::close_stdin() {
    if (!impl_->stdin_open) return;
    impl_->stdin_open = false;
    native::close_stdin(impl_->child);
}

bool Process::read_line(std::string& line) {
    for (;;) {
        std::size_t newline = impl_->stdout_buffer.find('\n');
        if (newline != std::string::npos) {
            line.assign(impl_->stdout_buffer, 0, newline);
            impl_->stdout_buffer.erase(0, newline + 1);
            return true;
        }
        if (impl_->stdout_eof) {
            if (impl_->stdout_buffer.empty()) return false;
            line.swap(impl_->stdout_buffer);  // last line without a newline
            impl_->stdout_buffer.clear();
            return true;
        }
        char buffer[4096];
        std::size_t count = native::read_stdout(impl_->child, buffer, sizeof(buffer));
        if (count == 0) {
            impl_->stdout_eof = true;
        } else {
            impl_->stdout_buffer.append(buffer, count);
        }
    }
}

std::string Process::read_all_stdout() {
    while (!impl_->stdout_eof) {
        char buffer[1 << 16];
        std::size_t count = native::read_stdout(impl_->child, buffer, sizeof(buffer));
        if (count == 0) {
            impl_->stdout_eof = true;
        } else {
            impl_->stdout_buffer.append(buffer, count);
        }
    }
    std::string out;
    out.swap(impl_->stdout_buffer);
    return out;
}

std::string Process::stderr_text() {
    if (impl_->stderr_thread.joinable() && impl_->exited) impl_->stderr_thread.join();
    return impl_->stderr_data;
}

bool Process::wait_for(double timeout_seconds) {
    {
        std::lock_guard<std::mutex> lock(impl_->state_mutex);
        if (impl_->exited) return true;
    }
    std::int64_t code = 0;
    if (!native::wait(impl_->child, timeout_seconds, &code)) return false;
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    impl_->exited = true;
    impl_->code = code;
    return true;
}

std::int64_t Process::wait() {
    wait_for(-1.0);
    if (impl_->stderr_thread.joinable()) impl_->stderr_thread.join();
    return impl_->code;
}

bool Process::running() { return !wait_for(0.0); }

std::int64_t Process::exit_code() const { return impl_->code; }

void Process::kill() {
    std::lock_guard<std::mutex> lock(impl_->state_mutex);
    if (impl_->exited) return;
    native::kill(impl_->child);
}

std::int64_t Process::pid() const { return impl_->pid; }

void ProcessGroup::add(Process* process) {
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.push_back(process);
}

void ProcessGroup::remove(Process* process) {
    std::lock_guard<std::mutex> lock(mutex_);
    processes_.erase(std::remove(processes_.begin(), processes_.end(), process), processes_.end());
}

void ProcessGroup::kill_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Process* process : processes_) process->kill();
}

RunResult run_capture(const StartOptions& given, const std::string& input, double timeout_seconds) {
    StartOptions options = given;
    options.pipe_stdin = true;
    options.pipe_stdout = true;
    if (options.error_stream == ErrorStream::Inherit) options.error_stream = ErrorStream::Pipe;
    std::unique_ptr<Process> process = Process::start(options);

    RunResult result;
    // Feed stdin and read stdout on helper threads so neither pipe can fill up and deadlock.
    std::thread feeder([&] {
        process->write_stdin(input);
        process->close_stdin();
    });
    std::thread reader([&] { result.out = process->read_all_stdout(); });
    if (timeout_seconds > 0 && !process->wait_for(timeout_seconds)) {
        result.timed_out = true;
        process->kill();
    }
    result.exit_code = process->wait();
    reader.join();
    feeder.join();
    result.err = process->stderr_text();
    return result;
}

}  // namespace dc_determinism
