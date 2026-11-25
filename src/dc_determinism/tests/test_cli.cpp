// The command-line parser: a table of good and bad invocations with their expected exit codes.
#include <sstream>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream stream(line);
    std::string word;
    while (stream >> word) out.push_back(word);
    return out;
}

// Parses `line` (whitespace-separated) and returns 0 (accepted), or the ParseExit code.
// Usage errors and --help print to the standard streams; that is expected here.
int outcome(const std::string& line) {
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    try {
        parser->parse(split(line));
        return 0;
    } catch (const cli::ParseExit& exit) {
        return exit.code == 0 ? 10 : exit.code;  // 10 = help was shown
    }
}

}  // namespace

TEST(accepted_invocations) {
    const char* good[] = {
        "prepare --output out",
        "prepare --output out --limit 0",
        "prepare --output out --limit 500 --logs-dir logs",
        "prepare --output=out --limit=5",
        "prepare --out out --lim 7",  // unique prefixes
        "prepare --output out --limit 1_0",
        "prepare --output out --limit +5",
        "verify",
        "verify --compiler msvc",
        "verify --compiler gcc --verbose",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r.json",
        "run simulator --logs-dir l --limit 3 --compiler gcc --engine fcv1plus --output r.json --repeats 3",
        "run simulator --prepared p --compiler clang --engine fcv1 --output r --workers 4 --max-steps 10 --timeout 1.5",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --compare-logs "
        "--compare-logs-basis finish --compare-logs-basis-fallback --compare-logs-tolerance 0.02",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --worker-command x",
        "run player --player nd2 --compiler wasm --output o.json",
        "run player --player normal_dist --compiler msvc --output o.json --repeats 5 --samples 640 --timeout 0",
        "run player --player nd2 --compiler gcc --output o.json --player-command x",
        "benchmark --prepared p --compiler gcc --output b.json",
        "benchmark --logs-dir l --compiler gcc --output b.json --repeats 1 --limit 5 --timeout 2",
        "capture simulator --prepared p --engine fcv1 --compiler msvc --all --output c.zip",
        "capture simulator --prepared p --engine pybind-py36 --shots ids.txt --logs-dir l --output c.zip",
        "capture simulator --prepared p --engine fcv1plus --all --expect r.json --output c.zip",
        "capture player --player nd2 --compiler wasm --output o.bin",
        "capture player --player normal_dist --compiler msvc --samples 640 --timeout 3 --output o.bin "
        "--expect r.json --player-command x",
        "compare simulator a.json b.json --output c.json",
        "compare simulator a.zip b.zip --output c.json --workers 2",
        "compare engines --prepared p --compiler msvc --output e.json",
        "compare engines --prepared p --compiler gcc --logs-dir l --compare-logs-tolerance 0 --output e.json",
        "compare pybind --prepared p --logs-dir l --compiler gcc --output e.json",
        "compare player l.bin r.bin --output c.json",
        "compare player l.json r.json --output c.json",
        "render trajectory a.zip --output o",
        "render trajectory a.zip b.zip --output o --shots ids.txt --contact-margin 0.01 --workers 2",
        "render trajectory a.zip --output o b.zip",  // options between the positionals
        "render trajectory a.zip --output o --contact-margin 0",
        "render trajectory a.zip --out o --con 0.02 --sh ids.txt",
    };
    for (const char* line : good) {
        int code = outcome(line);
        if (code != 0) ::testing::report_failure(__FILE__, __LINE__, std::string("rejected: ") + line);
    }
}

TEST(rejected_invocations) {
    const char* bad[] = {
        "",                                                   // a command is required
        "nosuchcommand",                                      // invalid choice
        "prepare",                                            // --output is required
        "prepare --output out --limit -1",                    // below the bound
        "prepare --output out --limit abc",
        "prepare --output out --limit 5.0",
        "prepare --output out --limit",                       // expected one argument
        "prepare --output out --bogus",                       // unrecognized
        "prepare --output out extra",                         // unrecognized positional
        "prepare --output out --l 5",                         // ambiguous: --logs-dir, --limit
        "verify --compiler icc",                              // invalid choice
        "verify --verbose=1",                                 // flag with an explicit value
        "run",                                                // sub-command required
        "run simulator --prepared p --compiler msvc --output r",             // --engine required
        "run simulator --prepared p --compiler msvc --engine fcv2 --output r",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --repeats 1",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --workers 0",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --max-steps 0",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --timeout -1",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --timeout nan",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --timeout inf",
        "run simulator --prepared p --compiler msvc --engine fcv1 --output r --compare-logs-basis other",
        "run simulator --prepared p --engine fcv1 --output r",               // --compiler required
        "run player --player nd2 --compiler wasm",                           // --output required
        "run player --player nd2 --compiler wasm --output o.json --expect r.json",   // capture player only
        "run player --player nd3 --compiler wasm --output o",
        "run player --player nd2 --compiler wasm --output o --samples 639",
        "run player --player nd2 --compiler wasm --output o --repeats 1",
        "benchmark --prepared p --compiler gcc",                             // --output required
        "benchmark --prepared p --compiler gcc --output b --repeats 0",
        "benchmark --logs-dir l --compiler gcc --output b.json --pybind-py27 x.so",   // removed option
        "benchmark --logs-dir l --compiler gcc --output b.json --pybind-py36 y.so",
        "run player --player nd2 --compiler wasm --output o.json --py36-rotation reversed",
        "capture",                                                           // sub-command required
        "capture --prepared p --engine fcv1 --all --output c.zip",           // the old bare `capture`
        "capture simulator --prepared p --engine fcv1 --output c.zip",       // --shots or --all
        "capture simulator --prepared p --engine fcv1 --all --shots x --output c.zip",  // mutually exclusive
        "capture simulator --prepared p --engine fcv3 --all --output c.zip",
        "capture simulator --engine fcv1 --all --output c.zip",              // --prepared required
        "capture simulator --prepared p --engine pybind-py36 --all --output c.zip --py36-rotation reversed",
        "capture simulator --prepared p --engine pybind-py36 --all --output c.zip --py36-rotation as-normalized",
        "capture simulator --prepared p --engine fcv1 --all --output c.zip --py36-rotation sideways",
        "capture player --player nd2 --compiler wasm",                       // --output required
        "capture player --player nd2 --output o.bin",                        // --compiler required
        "capture player --player nd2 --compiler wasm --output o.bin --repeats 2",  // run player only
        "capture player --player nd2 --compiler wasm --output o.bin --samples 639",
        "capture player --player nd3 --compiler wasm --output o.bin",
        "compare pybind --prepared p --logs-dir l --compiler gcc --output e.json --py36-rotation reversed",
        "render",                                                            // sub-command required
        "render trajectory --output o",                                      // A required
        "render trajectory a.zip",                                           // --output required
        "render trajectory a.zip b.zip c.zip --output o",                    // too many positionals
        "render trajectory a.zip --output o --contact-margin -0.5",
        "render trajectory a.zip --output o --contact-margin nan",
        "render trajectory a.zip --output o --workers 0",
        "render trajectory a.zip --output o --bogus",
        "compare",
        "compare simulator a.json --output c.json",                          // B missing
        "compare simulator a.json b.json c.json --output d.json",
        "compare simulator a.json b.json",                                   // --output required
        "compare engines --prepared p --output e.json",                      // --compiler required
        "compare pybind --prepared p --logs-dir l --compiler msvc --output e.json",  // gcc/clang only
        "compare pybind --prepared p --compiler gcc --output e.json",        // --logs-dir required
        "compare player l.bin --output c.json",
        "prepare --output out --dc-root dc",                                 // removed option
        "verify --compiler gcc --build",                                     // removed option
        "run simulator --prepared p --compiler gcc --engine fcv1 --output r --build",
        "run simulator --prepared p --compiler gcc --engine fcv1 --output r --dc-root dc",
        "run player --player nd2 --compiler gcc --output o.json --build",
        "capture simulator --prepared p --engine fcv1 --all --output c.zip --dc-root dc",
        "capture player --player nd2 --compiler gcc --output o.bin --dc-root dc",
        "compare simulator a.json b.json --output c.json --dc-root dc",
        "compare engines --prepared p --compiler gcc --output e.json --build",
        "compare pybind --prepared p --logs-dir l --compiler gcc --output e.json --dc-root dc",
        "compare player l.bin r.bin --output c.json --dc-root dc",
        "benchmark --prepared p --compiler gcc --output b.json --build",
        "--bogus",
    };
    for (const char* line : bad) {
        int code = outcome(line);
        if (code != 2) {
            ::testing::report_failure(__FILE__, __LINE__,
                                      std::string("not rejected with 2 (got ") + std::to_string(code) + "): " + line);
        }
    }
}

TEST(help_is_available_everywhere) {
    const char* helps[] = {
        "--help", "-h", "prepare --help", "verify -h", "run --help", "run simulator --help", "run player -h",
        "benchmark --help", "capture --help", "capture simulator --help", "capture player -h", "compare --help",
        "compare simulator --help",
        "compare engines --help", "compare pybind --help", "compare player --help", "render --help",
        "render trajectory --help",
    };
    for (const char* line : helps) {
        int code = outcome(line);
        if (code != 10) ::testing::report_failure(__FILE__, __LINE__, std::string("no help for: ") + line);
    }
}

TEST(defaults_and_types) {
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    cli::ParsedArgs args = parser->parse(
        split("run simulator --prepared p --compiler gcc --engine fcv1 --output r.json"));
    CHECK_EQ(cli::command_path(args), std::string("run simulator"));
    CHECK_EQ(args.integer("repeats"), static_cast<std::int64_t>(2));
    CHECK_EQ(args.integer("max_steps"), static_cast<std::int64_t>(60000));
    CHECK_EQ(args.integer("limit"), static_cast<std::int64_t>(0));
    CHECK_EQ(args.real("timeout"), 0.0);
    CHECK(args.integer("workers") >= 1);
    CHECK(!args.has("compare_logs_tolerance"));
    CHECK_EQ(args.text_or("prepared", ""), std::string("p"));
    CHECK(!args.has("logs_dir"));

    cli::ParsedArgs cap = parser->parse(split("capture simulator --prepared p --engine fcv1 --all --output c.zip"));
    CHECK_EQ(cli::command_path(cap), std::string("capture simulator"));
    CHECK(cap.flag("all"));

    cli::ParsedArgs run_player =
        parser->parse(split("run player --player nd2 --compiler gcc --output o.json"));
    CHECK_EQ(cli::command_path(run_player), std::string("run player"));
    CHECK_EQ(run_player.integer("repeats"), static_cast<std::int64_t>(2));
    CHECK_EQ(run_player.integer("samples"), static_cast<std::int64_t>(4096));
    CHECK(!run_player.has("expect"));

    cli::ParsedArgs capture_player =
        parser->parse(split("capture player --player nd2 --compiler gcc --output o.bin --expect r.json"));
    CHECK_EQ(cli::command_path(capture_player), std::string("capture player"));
    CHECK_EQ(capture_player.integer("samples"), static_cast<std::int64_t>(4096));
    CHECK_EQ(capture_player.text_or("expect", ""), std::string("r.json"));
    CHECK(!capture_player.has("repeats"));

    cli::ParsedArgs cmp = parser->parse(split("compare pybind --prepared p --logs-dir l --compiler gcc --output o"));
    CHECK_EQ(cmp.real("timeout"), 600.0);
    CHECK_EQ(cli::command_path(cmp), std::string("compare pybind"));

    cli::ParsedArgs one = parser->parse(split("render trajectory a.zip --output o"));
    CHECK_EQ(cli::command_path(one), std::string("render trajectory"));
    CHECK_EQ(one.text_or("a", ""), std::string("a.zip"));
    CHECK(!one.has("b"));
    CHECK(!one.has("shots"));
    CHECK_EQ(one.real("contact_margin"), 0.005);
    CHECK(one.integer("workers") >= 1);
    cli::ParsedArgs two = parser->parse(split("render trajectory a.zip --output o b.zip --contact-margin 0.01"));
    CHECK_EQ(two.text_or("a", ""), std::string("a.zip"));
    CHECK_EQ(two.text_or("b", ""), std::string("b.zip"));
    CHECK_EQ(two.real("contact_margin"), 0.01);
    CHECK(cli::find_handler("render trajectory") != nullptr);

    cli::ParsedArgs abbreviated = parser->parse(split("prepare --out o --lim=9"));
    CHECK_EQ(abbreviated.integer("limit"), static_cast<std::int64_t>(9));
}

TEST(an_optional_positional_is_shown_in_brackets_and_may_be_absent) {
    cli::Parser parser("prog");
    cli::Argument first;
    first.dest = "first";
    parser.add(first);
    cli::Argument second;
    second.dest = "second";
    second.optional_positional = true;
    parser.add(second);
    CHECK(parser.format_usage().find("prog [-h] FIRST [SECOND]") != std::string::npos);
    cli::ParsedArgs without = parser.parse({"x"});
    CHECK_EQ(without.text_or("first", ""), std::string("x"));
    CHECK(!without.has("second"));
    cli::ParsedArgs with = parser.parse({"x", "y"});
    CHECK_EQ(with.text_or("second", ""), std::string("y"));
    auto exit_code = [&](const std::vector<std::string>& args) {
        try {
            parser.parse(args);
            return 0;
        } catch (const cli::ParseExit& exit) {
            return exit.code;
        }
    };
    CHECK_EQ(exit_code({"x", "y", "z"}), 2);  // unrecognized argument
    CHECK_EQ(exit_code({}), 2);               // the first one is required
}

TEST(pybind_commands_are_part_of_the_cpp_binary) {
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    CHECK(cli::find_handler("compare pybind") != nullptr);
    CHECK(cli::find_handler("capture simulator") != nullptr);

    // a pybind capture is not turned away by the dispatcher any more: it asks for the logs behind --prepared
    cli::ParsedArgs pybind =
        parser->parse(split("capture simulator --prepared p --engine pybind-py27 --all --output c.zip"));
    bool asks_for_logs = false;
    try {
        cli::dispatch(pybind);
    } catch (const DcDeterminismError& error) {
        asks_for_logs = std::string(error.what()).find("requires --logs-dir") != std::string::npos;
    }
    CHECK(asks_for_logs);

    cli::ParsedArgs check = parser->parse(split("compare pybind --check-recording --prepared p --logs-dir l "
                                                "--removal-sheet-width shot --frame-stride 5 --limit 3 "
                                                "--engines py36 --output o"));
    CHECK(check.flag("check_recording"));
    CHECK_EQ(check.text_or("removal_sheet_width", ""), std::string("shot"));
    CHECK_EQ(check.integer("frame_stride"), static_cast<std::int64_t>(5));
    CHECK_EQ(check.integer("limit"), static_cast<std::int64_t>(3));
    CHECK_EQ(check.text_or("engines", ""), std::string("py36"));
    CHECK(!check.has("compiler"));
    cli::ParsedArgs plain = parser->parse(split("compare pybind --prepared p --logs-dir l --compiler gcc --output o"));
    CHECK(!plain.flag("check_recording"));
    CHECK(!plain.has("frame_stride") && !plain.has("limit") && !plain.has("removal_sheet_width") && !plain.has("engines"));
}

int main() { return testing::run_all_tests(); }
