#include "json/pyfloat.hpp"

#include <charconv>
#include <cmath>

namespace dc_determinism {

std::string py_float_repr(double value) {
    if (std::isnan(value)) return "nan";
    if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
    if (value == 0.0) return std::signbit(value) ? "-0.0" : "0.0";

    char buffer[64];
    std::to_chars_result result =
        std::to_chars(buffer, buffer + sizeof(buffer), std::fabs(value), std::chars_format::scientific);
    // buffer holds "d[.ddd]e(+|-)XX": shortest digits that round-trip, scientific layout.
    std::string digits;
    const char* cursor = buffer;
    for (; cursor != result.ptr && *cursor != 'e'; ++cursor) {
        if (*cursor != '.') digits.push_back(*cursor);
    }
    int exponent = 0;
    if (cursor != result.ptr) {
        ++cursor;  // past 'e'
        bool negative_exponent = cursor != result.ptr && *cursor == '-';
        if (cursor != result.ptr && (*cursor == '-' || *cursor == '+')) ++cursor;
        for (; cursor != result.ptr; ++cursor) exponent = exponent * 10 + (*cursor - '0');
        if (negative_exponent) exponent = -exponent;
    }

    // value = 0.<digits> * 10^decpt
    int decpt = exponent + 1;
    int count = static_cast<int>(digits.size());
    std::string out = value < 0 ? "-" : "";
    if (decpt <= -4 || decpt > 16) {
        out.push_back(digits[0]);
        if (count > 1) {
            out.push_back('.');
            out.append(digits, 1, std::string::npos);
        }
        int shown = decpt - 1;
        out.push_back('e');
        out.push_back(shown < 0 ? '-' : '+');
        int magnitude = shown < 0 ? -shown : shown;
        if (magnitude < 10) out.push_back('0');
        out += std::to_string(magnitude);
    } else if (decpt <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-decpt), '0');
        out += digits;
    } else if (decpt >= count) {
        out += digits;
        out.append(static_cast<std::size_t>(decpt - count), '0');
        out += ".0";
    } else {
        out.append(digits, 0, static_cast<std::size_t>(decpt));
        out.push_back('.');
        out.append(digits, static_cast<std::size_t>(decpt), std::string::npos);
    }
    return out;
}

}  // namespace dc_determinism
