// The few Python text and number semantics the corpus code depends on: strict UTF-8 decoding,
// `str.strip()`, `str.splitlines()`, `int()` / `float()` of a JSON value, `fnmatch`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "json/json.hpp"

namespace dc_determinism {

// `bytes.decode("utf-8")` succeeds (no overlong forms, no surrogates, nothing above U+10FFFF).
bool is_valid_utf8(const std::string& bytes);

// `not text.strip()`: the text is empty or only Python whitespace (`str.isspace`). UTF-8 input.
bool py_is_blank(const std::string& text);

// `str.splitlines()` (separators \n \r \r\n \v \f \x1c-\x1e U+0085 U+2028 U+2029, not kept).
// UTF-8 input.
std::vector<std::string> py_splitlines(const std::string& text);

// ASCII-only `str.lower()` (all the callers compare with ASCII words).
std::string ascii_lower(const std::string& text);

// `fnmatch.fnmatchcase(name, "shot_e*.json")`.
bool is_shot_name(const std::string& name);
// `name.lower().endswith(".zip")`.
bool is_zip_name(const std::string& name);

// Python `int(value)` of a decoded JSON value, restricted to what fits a signed 64-bit integer:
// ints, bools, floats (truncated; nan/inf rejected like int() does) and strings holding an
// integer. Returns false (with `reason`) when Python raises, or when the result does not fit.
bool py_int(const Json& value, std::int64_t& out, std::string& reason);

// Python `float(value)` of a decoded JSON value: ints, floats, bools and strings holding a number.
bool py_float(const Json& value, double& out, std::string& reason);

}  // namespace dc_determinism
