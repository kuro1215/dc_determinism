// A small argparse look-alike for the dc_determinism command line: long options with `--opt value` and
// `--opt=value`, unique-prefix abbreviations, typed and bounded values, choices, required options,
// mutually exclusive groups, nested sub-commands, argparse-style usage/help and error messages
// (`usage: ...` + `<prog>: error: <message>`, exit code 2).
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace dc_determinism {
namespace cli {

// Thrown after the parser has printed its help (code 0) or its usage error (code 2).
struct ParseExit {
    int code;
};

enum class ValueKind { String, Int, Float };

struct ValueType {
    ValueKind kind = ValueKind::String;
    std::string name;                 // used in messages: "<name> must be >= <minimum>"
    std::optional<double> minimum;    // Int / Float
};

struct Argument {
    std::vector<std::string> flags;   // empty for a positional argument
    std::string dest;
    std::string metavar;              // positional name or option value name (default: DEST upper-cased)
    std::string help;
    bool hidden = false;              // argparse.SUPPRESS
    bool required = false;
    bool store_true = false;
    bool append = false;              // a repeated option collects its values (joined by "\n") instead of replacing
    std::vector<std::string> choices;
    ValueType type;
    std::string group;                // title of the argument group the help lists it under
    std::string default_text;         // string default; empty: none
    std::optional<std::int64_t> default_int;
    std::optional<double> default_float;
};

using Value = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

class ParsedArgs {
public:
    bool has(const std::string& dest) const;  // given or defaulted
    bool flag(const std::string& dest) const; // store_true option
    std::optional<std::string> text(const std::string& dest) const;
    std::string text_or(const std::string& dest, const std::string& fallback) const;
    std::int64_t integer(const std::string& dest) const;
    double real(const std::string& dest) const;
    // Order in which options were given (for tests): dest names.
    const std::vector<std::string>& given() const { return given_; }

    void set(const std::string& dest, Value value, bool explicit_given);

private:
    std::map<std::string, Value> values_;
    std::vector<std::string> given_;
};

struct ArgumentGroup {
    std::string title;
    std::string description;
};

class Parser {
public:
    // `prog` is the full program name, e.g. "dc_determinism run simulator".
    Parser(std::string prog, std::string description = std::string(), std::string help = std::string());

    // The argument is stored in the parser; the returned reference stays valid until the next add.
    Argument& add(Argument argument);
    void add_group(const std::string& title, const std::string& description = std::string());
    // Arguments added from here on belong to this group until the next call ("" = default group).
    void use_group(const std::string& title);
    void add_exclusive(const std::vector<std::string>& dests, bool required);

    // A sub-command level: `dest` receives the chosen name. `required` as in argparse.
    Parser& add_subparsers(const std::string& dest, const std::string& title = std::string());
    Parser& add_command(const std::string& name, const std::string& help, const std::string& description = std::string());

    // Parses `args` (without the program name). Prints help (exit 0) or the error (exit 2) and
    // throws ParseExit.
    ParsedArgs parse(const std::vector<std::string>& args) const;

    std::string format_usage() const;
    std::string format_help() const;

private:
    struct Exclusive {
        std::vector<std::string> dests;
        bool required;
    };
    struct SubCommand {
        std::string name;
        std::string help;
        Parser* parser;
    };

    void parse_into(const std::vector<std::string>& args, ParsedArgs& out, std::vector<std::string>& extras) const;
    [[noreturn]] void fail(const std::string& message) const;
    const Argument* find_exact(const std::string& flag) const;
    std::string display_name(const Argument& argument) const;
    std::string usage_part(const Argument& argument) const;

    std::string prog_;
    std::string description_;
    std::string help_;
    std::vector<Argument> arguments_;
    std::vector<ArgumentGroup> groups_;
    std::string current_group_;
    std::vector<Exclusive> exclusives_;
    std::string sub_dest_;
    std::string sub_title_;
    std::vector<SubCommand> commands_;
    std::vector<std::unique_ptr<Parser>> owned_;
    friend class ParserTestAccess;
};

}  // namespace cli
}  // namespace dc_determinism
