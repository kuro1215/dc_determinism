#include "corpus/pytext.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace dc_determinism {

namespace {

// Decodes the code point at `index` (valid UTF-8 assumed by the callers that are not validating).
std::uint32_t decode_at(const std::string& text, std::size_t index, std::size_t& length) {
    unsigned char lead = static_cast<unsigned char>(text[index]);
    if (lead < 0x80) {
        length = 1;
        return lead;
    }
    length = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : 2;
    std::uint32_t code = lead & (0xffu >> (length + 1));
    for (std::size_t i = 1; i < length && index + i < text.size(); ++i) {
        code = (code << 6) | (static_cast<unsigned char>(text[index + i]) & 0x3fu);
    }
    return code;
}

bool is_py_space(std::uint32_t code) {
    if (code >= 0x09 && code <= 0x0d) return true;
    if (code >= 0x1c && code <= 0x20) return true;
    switch (code) {
        case 0x85: case 0xa0: case 0x1680: case 0x2028: case 0x2029: case 0x202f: case 0x205f:
        case 0x3000:
            return true;
        default:
            return code >= 0x2000 && code <= 0x200a;
    }
}

bool ascii_space(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

std::string trim_ascii(const std::string& text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && ascii_space(text[begin])) ++begin;
    while (end > begin && ascii_space(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

bool iequals(const std::string& text, const char* word) {
    std::size_t length = std::strlen(word);
    if (text.size() != length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != word[i]) return false;
    }
    return true;
}

}  // namespace

bool is_valid_utf8(const std::string& bytes) {
    std::size_t index = 0;
    const std::size_t size = bytes.size();
    while (index < size) {
        unsigned char lead = static_cast<unsigned char>(bytes[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        std::size_t length;
        std::uint32_t minimum;
        if (lead >= 0xc2 && lead <= 0xdf) {
            length = 2;
            minimum = 0x80;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            length = 3;
            minimum = 0x800;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            length = 4;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (index + length > size) return false;
        std::uint32_t code = lead & (0xffu >> (length + 1));
        for (std::size_t i = 1; i < length; ++i) {
            unsigned char next = static_cast<unsigned char>(bytes[index + i]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3fu);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
        index += length;
    }
    return true;
}

bool py_is_blank(const std::string& text) {
    std::size_t index = 0;
    while (index < text.size()) {
        std::size_t length = 1;
        std::uint32_t code = decode_at(text, index, length);
        if (!is_py_space(code)) return false;
        index += length;
    }
    return true;
}

std::vector<std::string> py_splitlines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    std::size_t index = 0;
    const std::size_t size = text.size();
    while (index < size) {
        unsigned char c = static_cast<unsigned char>(text[index]);
        std::size_t separator = 0;
        if (c == '\n' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e) {
            separator = 1;
        } else if (c == '\r') {
            separator = index + 1 < size && text[index + 1] == '\n' ? 2 : 1;
        } else if (c == 0xc2 && index + 1 < size && static_cast<unsigned char>(text[index + 1]) == 0x85) {
            separator = 2;
        } else if (c == 0xe2 && index + 2 < size && static_cast<unsigned char>(text[index + 1]) == 0x80 &&
                   (static_cast<unsigned char>(text[index + 2]) == 0xa8 ||
                    static_cast<unsigned char>(text[index + 2]) == 0xa9)) {
            separator = 3;
        }
        if (separator == 0) {
            ++index;
            continue;
        }
        lines.push_back(text.substr(start, index - start));
        index += separator;
        start = index;
    }
    if (start < size) lines.push_back(text.substr(start));
    return lines;
}

std::string ascii_lower(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool is_shot_name(const std::string& name) {
    static const char prefix[] = "shot_e";
    static const char suffix[] = ".json";
    const std::size_t prefix_size = sizeof(prefix) - 1;
    const std::size_t suffix_size = sizeof(suffix) - 1;
    if (name.size() < prefix_size + suffix_size) return false;
    return name.compare(0, prefix_size, prefix) == 0 &&
           name.compare(name.size() - suffix_size, suffix_size, suffix) == 0;
}

bool is_zip_name(const std::string& name) {
    if (name.size() < 4) return false;
    return ascii_lower(name.substr(name.size() - 4)) == ".zip";
}

bool py_int(const Json& value, std::int64_t& out, std::string& reason) {
    switch (value.type()) {
        case Json::value_t::number_integer:
            out = value.get<std::int64_t>();
            return true;
        case Json::value_t::number_unsigned: {
            std::uint64_t big = value.get<std::uint64_t>();
            if (big > static_cast<std::uint64_t>(INT64_MAX)) {
                reason = "end or shot out of range";
                return false;
            }
            out = static_cast<std::int64_t>(big);
            return true;
        }
        case Json::value_t::boolean:
            out = value.get<bool>() ? 1 : 0;
            return true;
        case Json::value_t::number_float: {
            double number = value.get<double>();
            if (std::isnan(number)) {
                reason = "cannot convert float NaN to integer";
                return false;
            }
            if (std::isinf(number)) {
                reason = "cannot convert float infinity to integer";
                return false;
            }
            double truncated = std::trunc(number);
            if (truncated >= 9223372036854775808.0 || truncated < -9223372036854775808.0) {
                reason = "end or shot out of range";
                return false;
            }
            out = static_cast<std::int64_t>(truncated);
            return true;
        }
        case Json::value_t::string: {
            std::string text = trim_ascii(value.get<std::string>());
            std::size_t index = 0;
            bool negative = false;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                negative = text[index] == '-';
                ++index;
            }
            if (index >= text.size()) {
                reason = "invalid literal for int()";
                return false;
            }
            std::uint64_t magnitude = 0;
            bool previous_digit = false;
            for (; index < text.size(); ++index) {
                char c = text[index];
                if (c == '_' && previous_digit && index + 1 < text.size() && text[index + 1] >= '0' &&
                    text[index + 1] <= '9') {
                    previous_digit = false;
                    continue;
                }
                if (c < '0' || c > '9') {
                    reason = "invalid literal for int()";
                    return false;
                }
                previous_digit = true;
                std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
                if (magnitude > (UINT64_MAX - digit) / 10) {
                    reason = "end or shot out of range";
                    return false;
                }
                magnitude = magnitude * 10 + digit;
            }
            if (negative ? magnitude > static_cast<std::uint64_t>(INT64_MAX) + 1 : magnitude > INT64_MAX) {
                reason = "end or shot out of range";
                return false;
            }
            out = negative ? static_cast<std::int64_t>(0 - magnitude) : static_cast<std::int64_t>(magnitude);
            return true;
        }
        default:
            reason = "int() argument must be a string, a bytes-like object or a real number";
            return false;
    }
}

bool py_float(const Json& value, double& out, std::string& reason) {
    switch (value.type()) {
        case Json::value_t::number_integer:
            out = static_cast<double>(value.get<std::int64_t>());
            return true;
        case Json::value_t::number_unsigned:
            out = static_cast<double>(value.get<std::uint64_t>());
            return true;
        case Json::value_t::number_float:
            out = value.get<double>();
            return true;
        case Json::value_t::boolean:
            out = value.get<bool>() ? 1.0 : 0.0;
            return true;
        case Json::value_t::string: {
            std::string text = trim_ascii(value.get<std::string>());
            std::size_t index = 0;
            bool negative = false;
            if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
                negative = text[index] == '-';
                ++index;
            }
            std::string rest = text.substr(index);
            if (iequals(rest, "inf") || iequals(rest, "infinity")) {
                out = negative ? -INFINITY : INFINITY;
                return true;
            }
            if (iequals(rest, "nan")) {
                out = NAN;
                return true;
            }
            std::size_t cursor = 0;
            std::size_t digits = 0;
            while (cursor < rest.size() && rest[cursor] >= '0' && rest[cursor] <= '9') ++cursor, ++digits;
            if (cursor < rest.size() && rest[cursor] == '.') {
                ++cursor;
                while (cursor < rest.size() && rest[cursor] >= '0' && rest[cursor] <= '9') ++cursor, ++digits;
            }
            bool valid = digits > 0;
            if (valid && cursor < rest.size() && (rest[cursor] == 'e' || rest[cursor] == 'E')) {
                ++cursor;
                if (cursor < rest.size() && (rest[cursor] == '+' || rest[cursor] == '-')) ++cursor;
                std::size_t exponent_digits = 0;
                while (cursor < rest.size() && rest[cursor] >= '0' && rest[cursor] <= '9') {
                    ++cursor;
                    ++exponent_digits;
                }
                valid = exponent_digits > 0;
            }
            if (!valid || cursor != rest.size()) {
                reason = "could not convert string to float";
                return false;
            }
            double number = std::strtod(rest.c_str(), nullptr);
            out = negative ? -number : number;
            return true;
        }
        default:
            reason = "float() argument must be a string or a real number";
            return false;
    }
}

}  // namespace dc_determinism
