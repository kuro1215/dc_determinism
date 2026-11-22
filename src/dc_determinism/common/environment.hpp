// Host description recorded in result files.
#pragma once

#include <optional>
#include <string>

#include "json/json.hpp"

namespace dc_determinism {
namespace environment {

// Keys, in order: os, os_version, kernel, machine, python,
// cpu_model, logical_cpus, container. `python` is null in this implementation (there is none);
// `implementation` ("dc_determinism") is appended so that a result file says which tool wrote it.
Json collect();

// Pieces, exposed for tests.
bool in_container();
// The CPU model of a `/proc/cpuinfo` text: its first "model name", else, when it has the aarch64 fields,
// "[Vendor ]CPU implementer 0xNN part 0xNNN" of the first core (vendor for implementers 0x61 Apple,
// 0x41 Arm, 0x00 emulated/unknown); nullopt when neither exists.
std::optional<std::string> cpu_model_from_cpuinfo(const std::string& text);

}  // namespace environment
}  // namespace dc_determinism
