// `json.loads` with CPython semantics (the C scanner, strict=True), for the shot logs and game
// logs:
//  * accepts the NaN / Infinity / -Infinity literals and any JSON text nlohmann/json rejects for
//    that reason; rejects control characters in strings, leading zeros, trailing commas and a BOM;
//  * duplicate object keys: the last value wins, at the position of the first;
//  * integers that fit a 64-bit integer stay integers, bigger ones become doubles (they are only
//    converted with `float()` or range-checked).
// The input must be valid UTF-8 (see is_valid_utf8): it is decoded before parsing.
//
// Strings with lone surrogate escapes (`\ud800`) are accepted like Python does and stored as
// WTF-8; nothing in the corpus contains them.
#pragma once

#include <map>
#include <string>
#include <initializer_list>

#include "json/json.hpp"

namespace dc_determinism {

// Selects the members of the parsed document that are kept. Everything else is still parsed and
// validated (an invalid document is an error either way) but not stored.
class JsonKeepTree {
public:
    struct Node {
        std::map<std::string, Node> children;
        bool all = false;  // keep the whole value at this node
    };

    // `add({"log", "trajectory", "frames"})` keeps the value at that object path; the objects
    // on the way keep only the listed members.
    void add(std::initializer_list<const char*> path);
    const Node& root() const { return root_; }

private:
    Node root_;
};

// Returns false and sets `error` ("<what> at char <n>") when `text` is not JSON for Python.
bool py_json_loads(const std::string& text, Json& out, std::string& error,
                   const JsonKeepTree* keep = nullptr);

}  // namespace dc_determinism
