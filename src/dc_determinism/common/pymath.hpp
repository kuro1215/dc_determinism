// The Python float operations the reported numbers depend on, reproduced bit for bit:
//  * `math.hypot(x, y)`: CPython's `vector_norm` (Modules/mathmodule.c, identical in 3.12 and 3.14;
//    an error-free two-product/sum algorithm, correctly rounded in practice, not libm's hypot);
//  * `round(x)`: round half to even (the default IEEE rounding mode, `std::nearbyint`).
// Nothing here may be compiled with floating-point contraction or reassociation (the build passes
// -ffp-contract=off to GCC/Clang; MSVC /fp:precise does not contract on x64).
#pragma once

#include <cstdint>

namespace dc_determinism {

// `math.hypot(x, y)` for two floats.
double py_hypot(double x, double y);

// `round(x)` of a float: the nearest integer, ties to even. Throws DcDeterminismError where Python raises
// (nan, inf) or when the result does not fit a signed 64-bit integer.
std::int64_t py_round(double x);

}  // namespace dc_determinism
