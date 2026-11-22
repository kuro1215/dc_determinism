#include "common/text.hpp"

#include <cstdio>

#include "json/pyfloat.hpp"

namespace dc_determinism {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

bool is_digit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

std::string py_strip(const std::string& text) {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && is_space(text[first])) ++first;
    while (last > first && is_space(text[last - 1])) --last;
    return text.substr(first, last - first);
}

std::vector<std::string> py_split(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_space(text[i])) ++i;
        std::size_t start = i;
        while (i < text.size() && !is_space(text[i])) ++i;
        if (i > start) out.push_back(text.substr(start, i - start));
    }
    return out;
}

bool is_hex64(const std::string& text) {
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool py_parse_int(const std::string& text, std::int64_t& out) {
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= text.size() || !is_digit(text[i])) return false;
    unsigned long long value = 0;
    const unsigned long long limit = negative ? 9223372036854775808ULL : 9223372036854775807ULL;
    bool previous_digit = false;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') {
            if (!previous_digit || i + 1 >= text.size() || !is_digit(text[i + 1])) return false;
            previous_digit = false;
            continue;
        }
        if (!is_digit(c)) return false;
        previous_digit = true;
        unsigned digit = static_cast<unsigned>(c - '0');
        if (value > (limit - digit) / 10) return false;
        value = value * 10 + digit;
    }
    out = negative ? static_cast<std::int64_t>(0ULL - value) : static_cast<std::int64_t>(value);
    return true;
}

bool py_truthy(const Json& value) {
    if (value.is_null()) return false;
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_number_unsigned()) return value.get<std::uint64_t>() != 0;
    if (value.is_number_integer()) return value.get<std::int64_t>() != 0;
    if (value.is_number_float()) return value.get<double>() != 0.0;
    if (value.is_string()) return !value.get_ref<const std::string&>().empty();
    if (value.is_array() || value.is_object()) return !value.empty();
    return true;
}

std::string py_repr(const Json& value) {
    if (value.is_null()) return "None";
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    if (value.is_number_unsigned()) return std::to_string(value.get<std::uint64_t>());
    if (value.is_number_integer()) return std::to_string(value.get<std::int64_t>());
    if (value.is_number_float()) return py_float_repr(value.get<double>());
    if (value.is_string()) {
        const std::string& text = value.get_ref<const std::string&>();
        bool has_single = text.find('\'') != std::string::npos;
        bool has_double = text.find('"') != std::string::npos;
        char quote = has_single && !has_double ? '"' : '\'';
        std::string out(1, quote);
        for (char c : text) {
            if (c == quote || c == '\\') {
                out += '\\';
                out += c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\r') {
                out += "\\r";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        out += quote;
        return out;
    }
    if (value.is_array()) {
        std::string out = "[";
        bool first = true;
        for (const Json& item : value) {
            if (!first) out += ", ";
            first = false;
            out += py_repr(item);
        }
        return out + "]";
    }
    std::string out = "{";
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!first) out += ", ";
        first = false;
        out += py_repr(Json(it.key())) + ": " + py_repr(it.value());
    }
    return out + "}";
}

std::string head_code_points(const std::string& text, std::size_t count) {
    std::size_t seen = 0;
    std::size_t end = 0;
    while (end < text.size() && seen < count) {
        ++end;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        ++seen;
    }
    return text.substr(0, end);
}

std::string format_g(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

std::string format_fixed(double value, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return buffer;
}

Json json_get(const Json& object, const char* key) { return json_get(object, key, Json(nullptr)); }

Json json_get(const Json& object, const char* key, const Json& fallback) {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    return it == object.end() ? fallback : *it;
}

}  // namespace dc_determinism
