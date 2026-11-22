#include "json/writer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "json/pyfloat.hpp"

namespace dc_determinism {

DumpOptions DumpOptions::pretty(int indent, bool allow_nan) {
    DumpOptions options;
    options.indent = indent;
    options.item_separator = ",";
    options.key_separator = ": ";
    options.allow_nan = allow_nan;
    return options;
}

DumpOptions DumpOptions::compact(bool sort_keys, bool allow_nan) {
    DumpOptions options;
    options.item_separator = ",";
    options.key_separator = ":";
    options.sort_keys = sort_keys;
    options.allow_nan = allow_nan;
    return options;
}

namespace {

void append_u_escape(std::string& out, unsigned code) {
    static const char* hex = "0123456789abcdef";
    out += "\\u";
    out.push_back(hex[(code >> 12) & 0xf]);
    out.push_back(hex[(code >> 8) & 0xf]);
    out.push_back(hex[(code >> 4) & 0xf]);
    out.push_back(hex[code & 0xf]);
}

// Decodes one UTF-8 sequence at `index`; an invalid byte b becomes the lone surrogate U+DC00+b
// (Python's surrogateescape, which os-level strings carry).
std::uint32_t next_code_point(const std::string& text, std::size_t& index) {
    unsigned char lead = static_cast<unsigned char>(text[index]);
    auto invalid = [&]() {
        ++index;
        return static_cast<std::uint32_t>(0xdc00u + lead);
    };
    if (lead < 0x80) {
        ++index;
        return lead;
    }
    std::size_t length = lead >= 0xf0 && lead <= 0xf4 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc2 ? 2 : 0;
    if (lead >= 0xf5 || length == 0 || index + length > text.size()) return invalid();
    std::uint32_t code = lead & (0xffu >> (length + 1));
    for (std::size_t i = 1; i < length; ++i) {
        unsigned char next = static_cast<unsigned char>(text[index + i]);
        if ((next & 0xc0) != 0x80) return invalid();
        code = (code << 6) | (next & 0x3fu);
    }
    static const std::uint32_t minimum[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (code < minimum[length] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return invalid();
    index += length;
    return code;
}

void append_string(std::string& out, const std::string& text, bool ensure_ascii) {
    out.push_back('"');
    std::size_t index = 0;
    while (index < text.size()) {
        std::size_t start = index;
        std::uint32_t code = next_code_point(text, index);
        switch (code) {
            case '"': out += "\\\""; continue;
            case '\\': out += "\\\\"; continue;
            case '\n': out += "\\n"; continue;
            case '\r': out += "\\r"; continue;
            case '\t': out += "\\t"; continue;
            case '\b': out += "\\b"; continue;
            case '\f': out += "\\f"; continue;
            default: break;
        }
        if (code >= 0x20 && code < 0x7f) {
            out.push_back(static_cast<char>(code));
        } else if (!ensure_ascii && code >= 0x7f && !(code >= 0xd800 && code <= 0xdfff)) {
            out.append(text, start, index - start);
        } else if (code < 0x10000) {
            append_u_escape(out, code);
        } else {
            std::uint32_t reduced = code - 0x10000;
            append_u_escape(out, 0xd800 | ((reduced >> 10) & 0x3ff));
            append_u_escape(out, 0xdc00 | (reduced & 0x3ff));
        }
    }
    out.push_back('"');
}

void append_float(std::string& out, double value, const DumpOptions& options) {
    if (std::isnan(value) || std::isinf(value)) {
        std::string text = std::isnan(value) ? "NaN" : value < 0 ? "-Infinity" : "Infinity";
        if (!options.allow_nan) {
            throw DcDeterminismError("Out of range float values are not JSON compliant: " +
                             py_float_repr(value));
        }
        out += text;
        return;
    }
    out += py_float_repr(value);
}

void newline(std::string& out, const DumpOptions& options, int depth) {
    out.push_back('\n');
    out.append(static_cast<std::size_t>(options.indent) * static_cast<std::size_t>(depth), ' ');
}

void encode(std::string& out, const Json& value, const DumpOptions& options, int depth) {
    switch (value.type()) {
        case Json::value_t::null: out += "null"; return;
        case Json::value_t::boolean: out += value.get<bool>() ? "true" : "false"; return;
        case Json::value_t::number_integer: out += std::to_string(value.get<std::int64_t>()); return;
        case Json::value_t::number_unsigned: out += std::to_string(value.get<std::uint64_t>()); return;
        case Json::value_t::number_float: append_float(out, value.get<double>(), options); return;
        case Json::value_t::string: append_string(out, value.get_ref<const std::string&>(), options.ensure_ascii); return;
        case Json::value_t::array: {
            if (value.empty()) {
                out += "[]";
                return;
            }
            out.push_back('[');
            bool first = true;
            for (const Json& item : value) {
                if (!first) out += options.item_separator;
                first = false;
                if (options.indent >= 0) newline(out, options, depth + 1);
                encode(out, item, options, depth + 1);
            }
            if (options.indent >= 0) newline(out, options, depth);
            out.push_back(']');
            return;
        }
        case Json::value_t::object: {
            if (value.empty()) {
                out += "{}";
                return;
            }
            std::vector<Json::const_iterator> order;
            for (Json::const_iterator it = value.begin(); it != value.end(); ++it) order.push_back(it);
            if (options.sort_keys) {
                std::sort(order.begin(), order.end(),
                          [](const Json::const_iterator& a, const Json::const_iterator& b) {
                              return a.key() < b.key();
                          });
            }
            out.push_back('{');
            bool first = true;
            for (const Json::const_iterator& it : order) {
                if (!first) out += options.item_separator;
                first = false;
                if (options.indent >= 0) newline(out, options, depth + 1);
                append_string(out, it.key(), options.ensure_ascii);
                out += options.key_separator;
                encode(out, it.value(), options, depth + 1);
            }
            if (options.indent >= 0) newline(out, options, depth);
            out.push_back('}');
            return;
        }
        default: throw DcDeterminismError("cannot serialize this JSON value");
    }
}

}  // namespace

std::string json_dumps(const Json& value, const DumpOptions& options) {
    std::string out;
    encode(out, value, options, 0);
    return out;
}

std::string json_pretty_file_text(const Json& value) {
    return json_dumps(value, DumpOptions::pretty(2, false)) + "\n";
}

Json parse_json_text(const std::string& text, const std::string& label) {
    try {
        return Json::parse(text);
    } catch (const Json::exception& error) {
        throw DcDeterminismError("malformed JSON in " + label + ": " + error.what());
    }
}

Json read_json(const fs::path& path) {
    return parse_json_text(read_file(path), path_to_utf8(path));
}

void write_json_new(const fs::path& path, const Json& value) {
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    refuse_existing(path);
    write_file_new(path, json_pretty_file_text(value));
}

}  // namespace dc_determinism
