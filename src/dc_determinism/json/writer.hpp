// `json.dumps` and `json.load` with CPython semantics for every value the tool writes.
#pragma once

#include <string>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

struct DumpOptions {
    int indent = -1;                   // < 0: single line (Python indent=None)
    std::string item_separator = ", ";  // Python default (indent=None)
    std::string key_separator = ": ";
    bool sort_keys = false;
    bool ensure_ascii = true;
    bool allow_nan = true;

    // json.dumps(value, indent=n[, allow_nan=...]): separators (",", ": ") with a line per item.
    static DumpOptions pretty(int indent, bool allow_nan = true);
    // json.dumps(value, separators=(",", ":"), sort_keys=...): no whitespace.
    static DumpOptions compact(bool sort_keys = false, bool allow_nan = true);
};

// Python `json.dumps(value, **options)`: float digits as `float.__repr__`, `ensure_ascii` string
// escaping (lower-case \uXXXX, surrogate pairs, DEL escaped), dict order = insertion order.
// A non-finite float with allow_nan=false: DcDeterminismError("Out of range float values are not JSON
// compliant: nan").
std::string json_dumps(const Json& value, const DumpOptions& options = DumpOptions());

// `json.dumps(value, indent=2, allow_nan=False) + "\n"` (what common.write_json_new writes).
std::string json_pretty_file_text(const Json& value);

// Python `json.load` of a file: DcDeterminismError "cannot read <path>: ..." or "malformed JSON in <path>: ...".
Json read_json(const fs::path& path);
Json parse_json_text(const std::string& text, const std::string& label);

// `common.write_json_new`: atomic, never overwrites ("refusing to overwrite existing output: ...").
void write_json_new(const fs::path& path, const Json& value);

}  // namespace dc_determinism
