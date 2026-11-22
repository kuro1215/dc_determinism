// The per-OS half of the process layer (process_windows.cpp, process_posix.cpp). Not for general use.
#pragma once

#include <cstddef>
#include <cstdint>

#include "process/process.hpp"

namespace dc_determinism {
namespace native {

struct Child;  // opaque, owned by the caller until destroy()

// Starts the program; throws DcDeterminismError("cannot start <program>: <reason>").
Child* spawn(const StartOptions& options, std::int64_t* pid);
// Blocking reads; return the number of bytes read, 0 at EOF (or when the stream does not exist).
std::size_t read_stdout(Child* child, char* buffer, std::size_t size);
std::size_t read_stderr(Child* child, char* buffer, std::size_t size);
// Blocking write of all bytes; false when the pipe is broken or closed.
bool write_stdin(Child* child, const char* data, std::size_t size);
void close_stdin(Child* child);
// Waits up to `timeout_seconds` (< 0: forever); true and the code when the process has exited.
bool wait(Child* child, double timeout_seconds, std::int64_t* code);
void kill(Child* child);
void destroy(Child* child);  // closes every remaining handle; the process must have exited

}  // namespace native
}  // namespace dc_determinism
