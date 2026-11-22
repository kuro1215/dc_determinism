#include "common/pymath.hpp"

#include <cfloat>
#include <cmath>
#include <limits>

#include "common/error.hpp"

namespace dc_determinism {

namespace {

struct DoubleLength {
    double hi;
    double lo;
};

// Algorithm 1.1 (requires |a| >= |b|).
inline DoubleLength dl_fast_sum(double a, double b) {
    double x = a + b;
    double y = (a - x) + b;
    return {x, y};
}

// Algorithm 3.5: an error-free product (`fma` is exact; CPython uses it unless UNRELIABLE_FMA, in
// which case Dekker's splitting yields the same exact pair).
inline DoubleLength dl_mul(double x, double y) {
    double z = x * y;
    double zz = std::fma(x, y, -z);
    return {z, zz};
}

// CPython `vector_norm` for n values already replaced by their absolute values.
double vector_norm(std::size_t n, double* vec, double max, bool found_nan) {
    if (std::isinf(max)) return max;
    if (found_nan) return std::numeric_limits<double>::quiet_NaN();
    if (max == 0.0 || n <= 1) return max;
    int max_e = 0;
    std::frexp(max, &max_e);
    if (max_e < -1023) {
        // ldexp(1.0, -max_e) would overflow: scale the subnormals up first.
        for (std::size_t i = 0; i < n; ++i) vec[i] /= DBL_MIN;
        return DBL_MIN * vector_norm(n, vec, max / DBL_MIN, found_nan);
    }
    double scale = std::ldexp(1.0, -max_e);
    double csum = 1.0;
    double frac1 = 0.0;
    double frac2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        double x = vec[i];
        x *= scale;                          // lossless scaling
        DoubleLength pr = dl_mul(x, x);      // lossless squaring
        DoubleLength sm = dl_fast_sum(csum, pr.hi);  // lossless addition
        csum = sm.hi;
        frac1 += pr.lo;                      // lossy addition
        frac2 += sm.lo;                      // lossy addition
    }
    double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
    DoubleLength pr = dl_mul(-h, h);
    DoubleLength sm = dl_fast_sum(csum, pr.hi);
    csum = sm.hi;
    frac1 += pr.lo;
    frac2 += sm.lo;
    double x = csum - 1.0 + (frac1 + frac2);
    h += x / (2.0 * h);  // differential correction
    return h / scale;
}

}  // namespace

double py_hypot(double x, double y) {
    double vec[2] = {std::fabs(x), std::fabs(y)};
    bool found_nan = std::isnan(vec[0]) || std::isnan(vec[1]);
    double max = 0.0;
    for (double value : vec) {
        if (value > max) max = value;
    }
    return vector_norm(2, vec, max, found_nan);
}

std::int64_t py_round(double x) {
    if (std::isnan(x)) throw DcDeterminismError("cannot convert float NaN to integer");
    if (std::isinf(x)) throw DcDeterminismError("cannot convert float infinity to integer");
    double rounded = std::nearbyint(x);
    // 2^63 is exactly representable; anything at or beyond it does not fit.
    if (rounded >= 9223372036854775808.0 || rounded < -9223372036854775808.0) {
        throw DcDeterminismError("rounded value out of range");
    }
    return static_cast<std::int64_t>(rounded);
}

}  // namespace dc_determinism
