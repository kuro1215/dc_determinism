#include "cli/argparse.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "common/platform.hpp"

namespace dc_determinism {
namespace cli {

namespace {

std::string py_repr(const std::string& text) {
    // Python repr() of a str, enough for command-line text: single quotes unless the text has
    // a single quote and no double quote.
    bool single = text.find('\'') != std::string::npos;
    bool dbl = text.find('"') != std::string::npos;
    char quote = single && !dbl ? '"' : '\'';
    std::string out(1, quote);
    for (char c : text) {
        if (c == '\\') {
            out += "\\\\";
        } else if (c == quote) {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

std::string join(const std::vector<std::string>& items, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += separator;
        out += items[i];
    }
    return out;
}

std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::string strip(const std::string& text) {
    std::size_t first = text.find_first_not_of(" \t\n\r\f\v");
    if (first == std::string::npos) return std::string();
    std::size_t last = text.find_last_not_of(" \t\n\r\f\v");
    return text.substr(first, last - first + 1);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// digit (["_"] digit)*  starting at `index`; advances it. Returns false when there is no digit.
bool digit_part(const std::string& text, std::size_t& index, std::string& digits) {
    if (index >= text.size() || !is_digit(text[index])) return false;
    while (index < text.size()) {
        if (is_digit(text[index])) {
            digits.push_back(text[index++]);
        } else if (text[index] == '_' && index + 1 < text.size() && is_digit(text[index + 1]) &&
                   !digits.empty()) {
            ++index;
        } else {
            break;
        }
    }
    return true;
}

// Python int(text) for values that fit std::int64_t.
bool python_int(const std::string& raw, std::int64_t& out) {
    std::string text = strip(raw);
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) negative = text[index++] == '-';
    std::string digits;
    if (!digit_part(text, index, digits) || index != text.size()) return false;
    // Values outside int64 are rejected (Python would accept arbitrarily large integers).
    const unsigned long long limit =
        negative ? (1ULL << 63) : static_cast<unsigned long long>(INT64_MAX);
    unsigned long long magnitude = 0;
    for (char c : digits) {
        unsigned long long digit = static_cast<unsigned long long>(c - '0');
        if (magnitude > (limit - digit) / 10) return false;
        magnitude = magnitude * 10 + digit;
    }
    out = negative ? static_cast<std::int64_t>(0 - magnitude) : static_cast<std::int64_t>(magnitude);
    return true;
}

bool iequals(const std::string& a, const char* b) {
    std::size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
    }
    return true;
}

// Python float(text).
bool python_float(const std::string& raw, double& out) {
    std::string text = strip(raw);
    std::size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) negative = text[index++] == '-';
    std::string body = text.substr(index);
    if (iequals(body, "inf") || iequals(body, "infinity")) {
        out = negative ? -HUGE_VAL : HUGE_VAL;
        return true;
    }
    if (iequals(body, "nan")) {
        out = std::nan("");
        return true;
    }
    std::string number = negative ? "-" : "";
    std::string integer;
    bool has_integer = digit_part(text, index, integer);
    number += integer;
    bool has_fraction = false;
    if (index < text.size() && text[index] == '.') {
        ++index;
        std::string fraction;
        has_fraction = digit_part(text, index, fraction);
        number += "." + fraction;
    }
    if (!has_integer && !has_fraction) return false;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        number += "e";
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) number.push_back(text[index++]);
        std::string exponent;
        if (!digit_part(text, index, exponent)) return false;
        number += exponent;
    }
    if (index != text.size()) return false;
    out = std::strtod(number.c_str(), nullptr);
    return true;
}

std::string format_number(double value) {
    char buffer[64];
    if (value == std::floor(value) && std::fabs(value) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%g", value);
    }
    return buffer;
}

int terminal_width() {
    int columns = 80;
    std::optional<std::string> text = env_get("COLUMNS");
    if (text) {
        std::int64_t value = 0;
        if (python_int(*text, value) && value > 0) columns = static_cast<int>(value);
    }
    return columns - 2;
}

std::vector<std::string> wrap_words(const std::string& text, int width) {
    std::vector<std::string> lines;
    std::istringstream words(text);
    std::string word;
    std::string line;
    while (words >> word) {
        if (!line.empty() && static_cast<int>(line.size() + 1 + word.size()) > width) {
            lines.push_back(line);
            line.clear();
        }
        if (!line.empty()) line += " ";
        line += word;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

bool looks_negative_number(const std::string& text) {
    // argparse: '^-\d+$|^-\d*\.\d+$'
    if (text.size() < 2 || text[0] != '-') return false;
    std::size_t i = 1;
    while (i < text.size() && is_digit(text[i])) ++i;
    if (i == text.size()) return true;
    if (text[i] != '.') return false;
    ++i;
    std::size_t start = i;
    while (i < text.size() && is_digit(text[i])) ++i;
    return i == text.size() && i > start;
}

}  // namespace

// ------------------------------------------------------------------------------ ParsedArgs

void ParsedArgs::set(const std::string& dest, Value value, bool explicit_given) {
    values_[dest] = std::move(value);
    if (explicit_given) given_.push_back(dest);
}

bool ParsedArgs::has(const std::string& dest) const {
    std::map<std::string, Value>::const_iterator it = values_.find(dest);
    return it != values_.end() && !std::holds_alternative<std::monostate>(it->second);
}

bool ParsedArgs::flag(const std::string& dest) const {
    std::map<std::string, Value>::const_iterator it = values_.find(dest);
    return it != values_.end() && std::holds_alternative<bool>(it->second) && std::get<bool>(it->second);
}

std::optional<std::string> ParsedArgs::text(const std::string& dest) const {
    std::map<std::string, Value>::const_iterator it = values_.find(dest);
    if (it == values_.end() || !std::holds_alternative<std::string>(it->second)) return std::nullopt;
    return std::get<std::string>(it->second);
}

std::string ParsedArgs::text_or(const std::string& dest, const std::string& fallback) const {
    return text(dest).value_or(fallback);
}

std::int64_t ParsedArgs::integer(const std::string& dest) const {
    std::map<std::string, Value>::const_iterator it = values_.find(dest);
    if (it == values_.end() || !std::holds_alternative<std::int64_t>(it->second)) return 0;
    return std::get<std::int64_t>(it->second);
}

double ParsedArgs::real(const std::string& dest) const {
    std::map<std::string, Value>::const_iterator it = values_.find(dest);
    if (it == values_.end() || !std::holds_alternative<double>(it->second)) return 0.0;
    return std::get<double>(it->second);
}

// ------------------------------------------------------------------------------ Parser

Parser::Parser(std::string prog, std::string description, std::string help)
    : prog_(std::move(prog)), description_(std::move(description)), help_(std::move(help)) {
    Argument help_option;
    help_option.flags = {"-h", "--help"};
    help_option.dest = "help";
    help_option.help = "show this help message and exit";
    help_option.store_true = true;
    arguments_.push_back(help_option);
}

Argument& Parser::add(Argument argument) {
    if (argument.dest.empty() && !argument.flags.empty()) {
        std::string flag = argument.flags.back();
        for (const std::string& candidate : argument.flags) {
            if (candidate.compare(0, 2, "--") == 0) {
                flag = candidate;
                break;
            }
        }
        std::string dest = flag.substr(flag.find_first_not_of('-'));
        std::replace(dest.begin(), dest.end(), '-', '_');
        argument.dest = dest;
    }
    if (argument.metavar.empty()) argument.metavar = upper(argument.dest);
    if (argument.group.empty()) argument.group = current_group_;
    arguments_.push_back(std::move(argument));
    return arguments_.back();
}

void Parser::add_group(const std::string& title, const std::string& description) {
    groups_.push_back({title, description});
    current_group_ = title;
}

void Parser::use_group(const std::string& title) { current_group_ = title; }

void Parser::add_exclusive(const std::vector<std::string>& dests, bool required) {
    exclusives_.push_back({dests, required});
}

Parser& Parser::add_subparsers(const std::string& dest, const std::string& title) {
    sub_dest_ = dest;
    sub_title_ = title;
    return *this;
}

Parser& Parser::add_command(const std::string& name, const std::string& help, const std::string& description) {
    owned_.emplace_back(new Parser(prog_ + " " + name, description.empty() ? help : description, help));
    commands_.push_back({name, help, owned_.back().get()});
    return *owned_.back();
}

const Argument* Parser::find_exact(const std::string& flag) const {
    for (const Argument& argument : arguments_) {
        if (std::find(argument.flags.begin(), argument.flags.end(), flag) != argument.flags.end()) {
            return &argument;
        }
    }
    return nullptr;
}

std::string Parser::display_name(const Argument& argument) const {
    if (argument.flags.empty()) return argument.metavar;
    return join(argument.flags, "/");
}

void Parser::fail(const std::string& message) const {
    std::fputs(format_usage().c_str(), stderr);
    std::fprintf(stderr, "%s: error: %s\n", prog_.c_str(), message.c_str());
    std::fflush(stderr);
    throw ParseExit{2};
}

namespace {

struct Classified {
    enum Kind { Positional, Option, Unknown } kind = Positional;
    const Argument* argument = nullptr;
    std::optional<std::string> explicit_value;
    std::string flag;  // as typed
};

}  // namespace

void Parser::parse_into(const std::vector<std::string>& args, ParsedArgs& out,
                        std::vector<std::string>& extras) const {
    std::vector<const Argument*> positionals;
    for (const Argument& argument : arguments_) {
        if (argument.flags.empty()) positionals.push_back(&argument);
    }
    std::size_t next_positional = 0;
    std::vector<const Argument*> seen;  // arguments seen so far (for exclusive groups)
    std::vector<const Argument*> given_options;

    auto classify = [&](const std::string& token) -> Classified {
        Classified result;
        if (token.empty() || token[0] != '-') return result;
        if (token == "--") {
            result.kind = Classified::Unknown;
            result.flag = token;
            return result;
        }
        if (const Argument* exact = find_exact(token)) {
            result.kind = Classified::Option;
            result.argument = exact;
            result.flag = token;
            return result;
        }
        if (token.size() == 1) return result;
        std::string flag = token;
        std::optional<std::string> explicit_value;
        std::size_t equals = token.find('=');
        if (equals != std::string::npos) {
            flag = token.substr(0, equals);
            explicit_value = token.substr(equals + 1);
            if (const Argument* exact = find_exact(flag)) {
                result.kind = Classified::Option;
                result.argument = exact;
                result.explicit_value = explicit_value;
                result.flag = flag;
                return result;
            }
        }
        if (token.compare(0, 2, "--") == 0) {
            std::vector<std::pair<const Argument*, std::string>> matches;
            for (const Argument& argument : arguments_) {
                for (const std::string& candidate : argument.flags) {
                    if (candidate.compare(0, 2, "--") == 0 && candidate.compare(0, flag.size(), flag) == 0) {
                        matches.emplace_back(&argument, candidate);
                    }
                }
            }
            if (matches.size() > 1) {
                std::vector<std::string> names;
                for (const std::pair<const Argument*, std::string>& match : matches) names.push_back(match.second);
                fail("ambiguous option: " + flag + " could match " + join(names, ", "));
            }
            if (matches.size() == 1) {
                result.kind = Classified::Option;
                result.argument = matches[0].first;
                result.explicit_value = explicit_value;
                result.flag = matches[0].second;
                return result;
            }
        }
        if (looks_negative_number(token)) return result;
        if (token.find(' ') != std::string::npos) return result;
        result.kind = Classified::Unknown;
        result.flag = token;
        return result;
    };

    auto convert = [&](const Argument& argument, const std::string& text, Value& value) {
        std::string label = "argument " + display_name(argument) + ": ";
        switch (argument.type.kind) {
            case ValueKind::String: value = text; break;
            case ValueKind::Int: {
                std::int64_t parsed = 0;
                if (!python_int(text, parsed)) {
                    fail(label + argument.type.name + " must be a number: " + py_repr(text));
                }
                if (argument.type.minimum && static_cast<double>(parsed) < *argument.type.minimum) {
                    fail(label + argument.type.name + " must be >= " + format_number(*argument.type.minimum));
                }
                value = parsed;
                break;
            }
            case ValueKind::Float: {
                double parsed = 0;
                if (!python_float(text, parsed)) {
                    fail(label + argument.type.name + " must be a number: " + py_repr(text));
                }
                if (!std::isfinite(parsed)) fail(label + argument.type.name + " must be finite: " + py_repr(text));
                if (argument.type.minimum && parsed < *argument.type.minimum) {
                    fail(label + argument.type.name + " must be >= " + format_number(*argument.type.minimum));
                }
                value = parsed;
                break;
            }
        }
        if (!argument.choices.empty() &&
            std::find(argument.choices.begin(), argument.choices.end(), text) == argument.choices.end()) {
            std::vector<std::string> shown;
            for (const std::string& choice : argument.choices) shown.push_back(py_repr(choice));
            fail(label + "invalid choice: " + py_repr(text) + " (choose from " + join(shown, ", ") + ")");
        }
    };

    auto check_exclusive = [&](const Argument& argument) {
        for (const Exclusive& group : exclusives_) {
            if (std::find(group.dests.begin(), group.dests.end(), argument.dest) == group.dests.end()) continue;
            for (const Argument* earlier : seen) {
                if (earlier != &argument &&
                    std::find(group.dests.begin(), group.dests.end(), earlier->dest) != group.dests.end()) {
                    fail("argument " + display_name(argument) + ": not allowed with argument " +
                         display_name(*earlier));
                }
            }
        }
    };

    bool only_positionals = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string& token = args[i];
        if (!only_positionals && token == "--") {
            // argparse drops the separator only when a positional consumes it; otherwise it is
            // reported as an unrecognized argument.
            only_positionals = true;
            bool consumed = next_positional < positionals.size() || (!commands_.empty() && !out.has(sub_dest_));
            if (!consumed) extras.push_back(token);
            continue;
        }
        Classified item;
        if (!only_positionals) item = classify(token);
        if (item.kind == Classified::Unknown) {
            extras.push_back(token);
            continue;
        }
        if (item.kind == Classified::Positional) {
            if (next_positional < positionals.size()) {
                const Argument& argument = *positionals[next_positional++];
                Value value;
                convert(argument, token, value);
                out.set(argument.dest, value, true);
                seen.push_back(&argument);
                continue;
            }
            if (!commands_.empty() && out.has(sub_dest_) == false) {
                std::vector<std::string> names;
                const SubCommand* chosen = nullptr;
                for (const SubCommand& command : commands_) {
                    names.push_back(py_repr(command.name));
                    if (command.name == token) chosen = &command;
                }
                if (chosen == nullptr) {
                    fail("argument " + sub_dest_ + ": invalid choice: " + py_repr(token) +
                         " (choose from " + join(names, ", ") + ")");
                }
                out.set(sub_dest_, token, true);
                std::vector<std::string> rest(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
                chosen->parser->parse_into(rest, out, extras);
                return;
            }
            extras.push_back(token);
            continue;
        }
        const Argument& argument = *item.argument;
        if (argument.dest == "help" && argument.flags.size() == 2 && argument.flags[0] == "-h") {
            std::fputs(format_help().c_str(), stdout);
            std::fflush(stdout);
            throw ParseExit{0};
        }
        check_exclusive(argument);
        if (argument.store_true) {
            if (item.explicit_value) {
                fail("argument " + display_name(argument) + ": ignored explicit argument " +
                     py_repr(*item.explicit_value));
            }
            out.set(argument.dest, true, true);
            seen.push_back(&argument);
            given_options.push_back(&argument);
            continue;
        }
        std::string text;
        if (item.explicit_value) {
            text = *item.explicit_value;
        } else {
            if (i + 1 >= args.size() ||
                (!only_positionals && classify(args[i + 1]).kind != Classified::Positional)) {
                fail("argument " + display_name(argument) + ": expected one argument");
            }
            text = args[++i];
        }
        Value value;
        convert(argument, text, value);
        if (argument.append && out.has(argument.dest) && std::holds_alternative<std::string>(value)) {
            value = out.text(argument.dest).value_or(std::string()) + "\n" + std::get<std::string>(value);
        }
        out.set(argument.dest, value, true);
        seen.push_back(&argument);
        given_options.push_back(&argument);
    }

    // Required arguments, then required exclusive groups (argparse order).
    std::vector<std::string> missing;
    for (const Argument& argument : arguments_) {
        bool is_required = argument.flags.empty() || argument.required;
        if (!is_required) continue;
        if (std::find(seen.begin(), seen.end(), &argument) == seen.end()) missing.push_back(display_name(argument));
    }
    if (!commands_.empty() && !out.has(sub_dest_)) missing.push_back(sub_dest_);
    if (!missing.empty()) fail("the following arguments are required: " + join(missing, ", "));
    for (const Exclusive& group : exclusives_) {
        if (!group.required) continue;
        bool any = false;
        std::vector<std::string> names;
        for (const std::string& dest : group.dests) {
            for (const Argument& argument : arguments_) {
                if (argument.dest != dest) continue;
                names.push_back(display_name(argument));
                if (std::find(seen.begin(), seen.end(), &argument) != seen.end()) any = true;
            }
        }
        if (!any) fail("one of the arguments " + join(names, " ") + " is required");
    }

    // Defaults for everything not given.
    for (const Argument& argument : arguments_) {
        if (argument.dest == "help") continue;
        bool given = std::find(seen.begin(), seen.end(), &argument) != seen.end();
        if (given) continue;
        if (argument.store_true) {
            out.set(argument.dest, false, false);
        } else if (argument.default_int) {
            out.set(argument.dest, *argument.default_int, false);
        } else if (argument.default_float) {
            out.set(argument.dest, *argument.default_float, false);
        } else if (!argument.default_text.empty()) {
            out.set(argument.dest, argument.default_text, false);
        }
    }
}

ParsedArgs Parser::parse(const std::vector<std::string>& args) const {
    ParsedArgs out;
    std::vector<std::string> extras;
    parse_into(args, out, extras);
    if (!extras.empty()) fail("unrecognized arguments: " + join(extras, " "));
    return out;
}

// ------------------------------------------------------------------------------ usage and help

std::string Parser::usage_part(const Argument& argument) const {
    std::string value;
    if (!argument.choices.empty()) {
        value = "{" + join(argument.choices, ",") + "}";
    } else {
        value = argument.metavar;
    }
    if (argument.flags.empty()) return value;
    std::string flag = argument.flags[0];
    std::string part = argument.store_true ? flag : flag + " " + value;
    return argument.required ? part : "[" + part + "]";
}

std::string Parser::format_usage() const {
    std::vector<std::string> optionals;
    std::vector<std::string> positional_parts;
    std::vector<bool> placed(arguments_.size(), false);
    for (std::size_t i = 0; i < arguments_.size(); ++i) {
        const Argument& argument = arguments_[i];
        if (argument.hidden || placed[i]) continue;
        if (argument.flags.empty()) {
            positional_parts.push_back(usage_part(argument));
            placed[i] = true;
            continue;
        }
        // Mutually exclusive groups are shown as (a | b) or [a | b] at the first member.
        bool grouped = false;
        for (const Exclusive& group : exclusives_) {
            if (std::find(group.dests.begin(), group.dests.end(), argument.dest) == group.dests.end()) continue;
            std::vector<std::string> parts;
            for (std::size_t j = 0; j < arguments_.size(); ++j) {
                if (std::find(group.dests.begin(), group.dests.end(), arguments_[j].dest) == group.dests.end()) continue;
                Argument copy = arguments_[j];
                copy.required = true;
                parts.push_back(usage_part(copy));
                placed[j] = true;
            }
            optionals.push_back(group.required ? "(" + join(parts, " | ") + ")" : "[" + join(parts, " | ") + "]");
            grouped = true;
            break;
        }
        if (grouped) continue;
        optionals.push_back(usage_part(argument));
        placed[i] = true;
    }
    if (!commands_.empty()) {
        std::vector<std::string> names;
        for (const SubCommand& command : commands_) names.push_back(command.name);
        positional_parts.push_back("{" + join(names, ",") + "}");
        positional_parts.push_back("...");
    }

    const std::string prefix = "usage: ";
    int text_width = std::max(terminal_width(), 11);
    std::vector<std::string> all = optionals;
    all.insert(all.end(), positional_parts.begin(), positional_parts.end());
    std::string one_line = prog_ + (all.empty() ? "" : " " + join(all, " "));
    if (static_cast<int>(prefix.size() + one_line.size()) <= text_width) return prefix + one_line + "\n";

    auto get_lines = [&](const std::vector<std::string>& parts, const std::string& indent,
                         const std::string* line_prefix) {
        std::vector<std::string> lines;
        std::vector<std::string> line;
        int indent_length = static_cast<int>(indent.size());
        int line_len = line_prefix ? static_cast<int>(line_prefix->size()) - 1 : indent_length - 1;
        for (const std::string& part : parts) {
            if (line_len + 1 + static_cast<int>(part.size()) > text_width && !line.empty()) {
                lines.push_back(indent + join(line, " "));
                line.clear();
                line_len = indent_length - 1;
            }
            line.push_back(part);
            line_len += static_cast<int>(part.size()) + 1;
        }
        if (!line.empty()) lines.push_back(indent + join(line, " "));
        if (line_prefix && !lines.empty()) lines[0] = lines[0].substr(indent.size());
        return lines;
    };

    std::vector<std::string> lines;
    if (prefix.size() + prog_.size() <= static_cast<std::size_t>(0.75 * text_width)) {
        std::string indent(prefix.size() + prog_.size() + 1, ' ');
        if (!optionals.empty()) {
            std::vector<std::string> head = {prog_};
            head.insert(head.end(), optionals.begin(), optionals.end());
            lines = get_lines(head, indent, &prefix);
            std::vector<std::string> more = get_lines(positional_parts, indent, nullptr);
            lines.insert(lines.end(), more.begin(), more.end());
        } else if (!positional_parts.empty()) {
            std::vector<std::string> head = {prog_};
            head.insert(head.end(), positional_parts.begin(), positional_parts.end());
            lines = get_lines(head, indent, &prefix);
        } else {
            lines = {prog_};
        }
    } else {
        std::string indent(prefix.size(), ' ');
        lines = get_lines(all, indent, nullptr);
        lines.insert(lines.begin(), prog_);
    }
    return prefix + join(lines, "\n") + "\n";
}

namespace {

struct HelpEntry {
    std::string invocation;
    std::string help;
    int indent;
};

}  // namespace

std::string Parser::format_help() const {
    int width = terminal_width();
    std::string out = format_usage();

    if (!description_.empty()) {
        out += "\n";
        for (const std::string& line : wrap_words(description_, width)) out += line + "\n";
    }

    auto invocation_of = [&](const Argument& argument) {
        if (argument.flags.empty()) return argument.choices.empty() ? argument.metavar : "{" + join(argument.choices, ",") + "}";
        std::string value = argument.choices.empty() ? argument.metavar : "{" + join(argument.choices, ",") + "}";
        std::vector<std::string> pieces;
        for (const std::string& flag : argument.flags) pieces.push_back(argument.store_true ? flag : flag + " " + value);
        return join(pieces, ", ");
    };

    struct Section {
        std::string title;
        std::string description;
        std::vector<HelpEntry> entries;
    };
    std::vector<Section> sections;
    Section positional_section{"positional arguments", "", {}};
    Section option_section{"options", "", {}};
    std::vector<Section> custom;
    for (const ArgumentGroup& group : groups_) custom.push_back({group.title, group.description, {}});

    for (const Argument& argument : arguments_) {
        if (argument.hidden) continue;
        HelpEntry entry{invocation_of(argument), argument.help, 2};
        if (argument.flags.empty()) {
            positional_section.entries.push_back(entry);
        } else if (!argument.group.empty()) {
            for (Section& section : custom) {
                if (section.title == argument.group) section.entries.push_back(entry);
            }
        } else {
            option_section.entries.push_back(entry);
        }
    }
    if (!commands_.empty()) {
        std::vector<std::string> names;
        for (const SubCommand& command : commands_) names.push_back(command.name);
        positional_section.entries.push_back({"{" + join(names, ",") + "}", "", 2});
        for (const SubCommand& command : commands_) {
            positional_section.entries.push_back({command.name, command.help, 4});
        }
    }
    if (!positional_section.entries.empty()) sections.push_back(positional_section);
    sections.push_back(option_section);
    for (const Section& section : custom) sections.push_back(section);

    int max_length = 0;
    for (const Section& section : sections) {
        for (const HelpEntry& entry : section.entries) {
            max_length = std::max(max_length, static_cast<int>(entry.invocation.size()) + entry.indent);
        }
    }
    int help_position = std::min(max_length + 2, 24);
    int help_width = std::max(width - help_position, 11);

    for (const Section& section : sections) {
        if (section.entries.empty()) continue;
        out += "\n" + section.title + ":\n";
        if (!section.description.empty()) {
            for (const std::string& line : wrap_words(section.description, width - 2)) out += "  " + line + "\n";
            out += "\n";
        }
        for (const HelpEntry& entry : section.entries) {
            std::string indent(static_cast<std::size_t>(entry.indent), ' ');
            int action_width = help_position - entry.indent - 2;
            std::vector<std::string> help_lines = wrap_words(entry.help, help_width);
            if (help_lines.empty()) {
                out += indent + entry.invocation + "\n";
                continue;
            }
            std::size_t first = 0;
            if (static_cast<int>(entry.invocation.size()) <= action_width) {
                std::string padded = entry.invocation;
                padded.append(static_cast<std::size_t>(action_width) - entry.invocation.size(), ' ');
                out += indent + padded + "  " + help_lines[0] + "\n";
                first = 1;
            } else {
                out += indent + entry.invocation + "\n";
            }
            for (std::size_t i = first; i < help_lines.size(); ++i) {
                out += std::string(static_cast<std::size_t>(help_position), ' ') + help_lines[i] + "\n";
            }
        }
    }
    return out;
}

}  // namespace cli
}  // namespace dc_determinism
