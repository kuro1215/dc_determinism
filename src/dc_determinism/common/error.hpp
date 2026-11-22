// An expected failure: reported as `error: ...` with exit code 2.
#pragma once

#include <stdexcept>
#include <string>

namespace dc_determinism {

class DcDeterminismError : public std::runtime_error {
public:
    explicit DcDeterminismError(const std::string& message) : std::runtime_error(message) {}
};

}  // namespace dc_determinism
