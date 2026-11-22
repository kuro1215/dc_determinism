// Small Python text/number semantics shared by the simulator commands: `str.strip()`,
// `str.split()`, `int(str)`, truthiness and `repr` of decoded JSON values, `%g`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

// `text.strip()` / `text.split()` (ASCII whitespace only: the worker protocol is ASCII).
std::string py_strip(const std::string& text);
std::vector<std::string> py_split(const std::string& text);

// `HEX64.fullmatch(text)`: exactly 64 lower-case hexadecimal digits.
bool is_hex64(const std::string& text);

// `int(text)` restricted to the signed 64-bit range: optional sign, digits with single
// underscores between digits. Returns false where Python raises ValueError (or the value does
// not fit).
bool py_parse_int(const std::string& text, std::int64_t& out);

// Python truthiness of a decoded JSON value (None, False, 0, 0.0, "", [] and {} are falsy).
bool py_truthy(const Json& value);

// `repr(value)` of a decoded JSON value (str with single quotes, None/True/False, numbers; lists
// and dicts recursively).
std::string py_repr(const Json& value);

// The first `count` code points of a UTF-8 string (Python `text[:count]`).
std::string head_code_points(const std::string& text, std::size_t count);

// Python `format(value, "g")`.
std::string format_g(double value);
// Python `f"{value:.<digits>f}"`.
std::string format_fixed(double value, int digits);

// `dict.get(key)`: the member, or null when `object` is not an object or lacks the key.
Json json_get(const Json& object, const char* key);
Json json_get(const Json& object, const char* key, const Json& fallback);

// The path as shown in Python messages (`str(Path)`).
inline std::string path_text(const fs::path& path) { return path_to_utf8(path); }

}  // namespace dc_determinism
