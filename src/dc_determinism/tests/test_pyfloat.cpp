// float repr against CPython's repr (the table was generated with CPython 3.14).
#include <cstdint>
#include <cstring>
#include <limits>

#include "json/pyfloat.hpp"
#include "testing.hpp"

namespace {

const struct {
    std::uint64_t bits;
    const char* text;
} kCases[] = {
    {0x0000000000000000ULL, "0.0"},
    {0x8000000000000000ULL, "-0.0"},
    {0x3ff0000000000000ULL, "1.0"},
    {0xbff0000000000000ULL, "-1.0"},
    {0x3fb999999999999aULL, "0.1"},
    {0x3fe0000000000000ULL, "0.5"},
    {0x4059000000000000ULL, "100.0"},
    {0x3f1a36e2eb1c432dULL, "0.0001"},
    {0x3f1a36e2eb1c432cULL, "9.999999999999999e-05"},
    {0x3ee4f8b588e368f1ULL, "1e-05"},
    {0x3eef75104d551d69ULL, "1.5e-05"},
    {0x3eb0c6f7a0b5ed8dULL, "1e-06"},
    {0x3e7ad7f29abcaf48ULL, "1e-07"},
    {0x3c36b082c2148b8eULL, "1.23e-18"},
    {0x430c6bf526340000ULL, "1000000000000000.0"},
    {0x430c6bf52633ffffULL, "999999999999999.9"},
    {0x4341c37937e08000ULL, "1e+16"},
    {0x4341c37937e07fffULL, "9999999999999998.0"},
    {0x4341c37937e08001ULL, "1.0000000000000002e+16"},
    {0x43e56a95319d63e1ULL, "1.2345678901234567e+19"},
    {0x4480f0cf064dd592ULL, "1e+22"},
    {0x44b52d02c7e14af6ULL, "1e+23"},
    {0x7fefffffffffffffULL, "1.7976931348623157e+308"},
    {0x0000000000000001ULL, "5e-324"},
    {0x0000000000000002ULL, "1e-323"},
    {0x0010000000000000ULL, "2.2250738585072014e-308"},
    {0x000fffffffffffffULL, "2.225073858507201e-308"},
    {0x419d6f34547e6b75ULL, "123456789.12345679"},
    {0x3fd5555555555555ULL, "0.3333333333333333"},
    {0x400921fb54442d18ULL, "3.141592653589793"},
    {0xc005bf0a8b145769ULL, "-2.718281828459045"},
    {0x4340000000000000ULL, "9007199254740992.0"},
    {0x43e0000000000000ULL, "9.223372036854776e+18"},
    {0x3fd3333333333334ULL, "0.30000000000000004"},
    {0x444b1ae4d6e2ef50ULL, "1e+21"},
    {0x3ddb7cdfd9d7bdbbULL, "1e-10"},
    {0x4011666666666666ULL, "4.35"},
    {0x3fb1eb851eb851ecULL, "0.07"},
    {0x40934a456d5cfaadULL, "1234.5678"},
    {0x3f0a36e2eb1c432dULL, "5e-05"},
    {0x3f202e4b6ce5dc68ULL, "0.00012345"},
    {0x447c7e83209e90b2ULL, "8.41e+21"},
};

double from_bits(std::uint64_t bits) {
    double value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

}  // namespace

TEST(table_from_cpython) {
    for (const auto& item : kCases) {
        CHECK_EQ(dc_determinism::py_float_repr(from_bits(item.bits)), std::string(item.text));
    }
}

TEST(non_finite) {
    CHECK_EQ(dc_determinism::py_float_repr(std::numeric_limits<double>::infinity()), std::string("inf"));
    CHECK_EQ(dc_determinism::py_float_repr(-std::numeric_limits<double>::infinity()), std::string("-inf"));
    CHECK_EQ(dc_determinism::py_float_repr(std::numeric_limits<double>::quiet_NaN()), std::string("nan"));
}

TEST(exponent_boundaries) {
    // decimal exponent < -4 or >= 16 switches to exponent notation
    CHECK_EQ(dc_determinism::py_float_repr(1e-4), std::string("0.0001"));
    CHECK_EQ(dc_determinism::py_float_repr(1e-5), std::string("1e-05"));
    CHECK_EQ(dc_determinism::py_float_repr(1e15), std::string("1000000000000000.0"));
    CHECK_EQ(dc_determinism::py_float_repr(1e16), std::string("1e+16"));
    CHECK_EQ(dc_determinism::py_float_repr(-1e16), std::string("-1e+16"));
    CHECK_EQ(dc_determinism::py_float_repr(1e100), std::string("1e+100"));
    CHECK_EQ(dc_determinism::py_float_repr(1e-100), std::string("1e-100"));
}

int main() { return testing::run_all_tests(); }
