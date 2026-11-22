// CPython `float.__repr__` (the digits json.dumps writes for a float).
#pragma once

#include <string>

namespace dc_determinism {

// Shortest round-trip digits (std::to_chars) laid out by CPython's repr rules: exponent form when
// the decimal exponent is < -4 or >= 16 (`1e-05`, `1e+16`), `-0.0`, otherwise positional with at
// least one fractional digit (`1.0`). Non-finite values give `inf`, `-inf` and `nan`.
std::string py_float_repr(double value);

}  // namespace dc_determinism
