// The process layer, tested against this executable itself in a few "--child <mode>" roles.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "common/error.hpp"
#include "common/platform.hpp"
#include "process/process.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

StartOptions child(const std::vector<std::string>& arguments) {
    StartOptions options;
    options.argv.push_back(path_to_utf8(executable_path()));
    options.argv.push_back("--child");
    for (const std::string& argument : arguments) options.argv.push_back(argument);
    return options;
}

int run_child(const std::vector<std::string>& args) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const std::string mode = args.size() > 2 ? args[2] : "";
    if (mode == "echo") {  // upper-case every stdin line, flush each answer
        std::string line;
        while (std::getline(std::cin, line)) {
            for (char& c : line) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            std::printf("%s\n", line.c_str());
            std::fflush(stdout);
        }
        return 0;
    }
    if (mode == "exit") return std::stoi(args[3]);
    if (mode == "sleep") {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        return 0;
    }
    if (mode == "streams") {
        std::fprintf(stdout, "to-stdout\n");
        std::fprintf(stderr, "to-stderr\n");
        return 0;
    }
    if (mode == "args") {
        for (std::size_t i = 3; i < args.size(); ++i) std::printf("[%s]\n", args[i].c_str());
        return 0;
    }
    if (mode == "env") {
        std::optional<std::string> value = env_get(args[3]);
        std::printf("%s\n", value ? value->c_str() : "<unset>");
        return 0;
    }
    if (mode == "cwd") {
        std::printf("%s\n", path_to_utf8(fs::current_path()).c_str());
        return 0;
    }
    if (mode == "big") {  // more output than a pipe buffer holds, stdout and stderr
        std::string chunk(1024, 'x');
        for (int i = 0; i < 2048; ++i) {
            std::fwrite(chunk.data(), 1, chunk.size(), stdout);
            std::fwrite(chunk.data(), 1, chunk.size(), stderr);
        }
        return 0;
    }
    return 99;
}

}  // namespace

TEST(line_protocol) {
    StartOptions options = child({"echo"});
    options.pipe_stdin = true;
    options.pipe_stdout = true;
    std::unique_ptr<Process> process = Process::start(options);
    std::string line;
    for (int i = 0; i < 3; ++i) {
        CHECK(process->write_stdin("hello " + std::to_string(i) + "\n"));
        CHECK(process->read_line(line));
        CHECK_EQ(line, "HELLO " + std::to_string(i));
    }
    process->close_stdin();
    CHECK(!process->read_line(line));
    CHECK_EQ(process->wait(), static_cast<std::int64_t>(0));
}

TEST(exit_codes) {
    for (int code : {0, 1, 2, 7}) {
        std::unique_ptr<Process> process = Process::start(child({"exit", std::to_string(code)}));
        CHECK_EQ(process->wait(), static_cast<std::int64_t>(code));
    }
}

TEST(missing_program) {
    StartOptions options;
    options.argv = {"dc_determinism-no-such-program-xyz"};
    CHECK_THROWS(Process::start(options));
}

TEST(kill_and_timeout) {
    std::unique_ptr<Process> process = Process::start(child({"sleep"}));
    CHECK(!process->wait_for(0.2));
    CHECK(process->running());
    process->kill();
    CHECK(process->wait_for(10.0));
    CHECK(process->wait() != 0);
    CHECK(!process->running());
}

TEST(kill_unblocks_a_reader) {
    StartOptions options = child({"sleep"});
    options.pipe_stdin = true;
    options.pipe_stdout = true;
    std::unique_ptr<Process> process = Process::start(options);
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        process->kill();
    });
    std::string line;
    CHECK(!process->read_line(line));  // EOF once the child is gone
    killer.join();
    process->wait();
}

TEST(group_kill_all) {
    std::vector<std::unique_ptr<Process>> processes;
    ProcessGroup group;
    for (int i = 0; i < 3; ++i) {
        processes.push_back(Process::start(child({"sleep"})));
        group.add(processes.back().get());
    }
    group.kill_all();
    for (std::unique_ptr<Process>& process : processes) CHECK(process->wait_for(10.0));
}

TEST(run_capture_collects_both_streams) {
    RunResult result = run_capture(child({"streams"}), "", 30.0);
    CHECK_EQ(result.exit_code, static_cast<std::int64_t>(0));
    CHECK(!result.timed_out);
    CHECK(result.out.find("to-stdout") == 0);
    CHECK(result.err.find("to-stderr") == 0);
}

TEST(run_capture_large_output_does_not_deadlock) {
    RunResult result = run_capture(child({"big"}), "", 60.0);
    CHECK_EQ(result.exit_code, static_cast<std::int64_t>(0));
    CHECK_EQ(result.out.size(), static_cast<std::size_t>(2048 * 1024));
    CHECK_EQ(result.err.size(), static_cast<std::size_t>(2048 * 1024));
}

TEST(run_capture_timeout) {
    RunResult result = run_capture(child({"sleep"}), "", 0.3);
    CHECK(result.timed_out);
    CHECK(result.exit_code != 0);
}

TEST(run_capture_feeds_stdin) {
    RunResult result = run_capture(child({"echo"}), "abc\ndef\n", 30.0);
    CHECK_EQ(result.out, std::string("ABC\nDEF\n"));
}

TEST(arguments_with_spaces_quotes_and_unicode) {
    std::vector<std::string> tricky = {"plain",       "with space", "quote\"inside", "back\\slash",
                                       "trailing\\",  "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88",
                                       "a\\\"b",      "two  spaces", ""};
    std::vector<std::string> arguments = {"args"};
    arguments.insert(arguments.end(), tricky.begin(), tricky.end());
    RunResult result = run_capture(child(arguments), "", 30.0);
    std::string expected;
    for (const std::string& item : tricky) expected += "[" + item + "]\n";
    CHECK_EQ(result.out, expected);
}

TEST(environment_changes) {
    StartOptions options = child({"env", "DC_DETERMINISM_TEST_VARIABLE"});
    options.environment = {{"DC_DETERMINISM_TEST_VARIABLE", "value one"}};
    CHECK_EQ(run_capture(options, "", 30.0).out, std::string("value one\n"));
    options.environment = {{"DC_DETERMINISM_TEST_VARIABLE", ""}};
    CHECK_EQ(run_capture(options, "", 30.0).out, std::string("<unset>\n"));
    // the parent environment is untouched
    CHECK(!env_get("DC_DETERMINISM_TEST_VARIABLE").has_value());
}

TEST(working_directory) {
    StartOptions options = child({"cwd"});
    fs::path directory = fs::canonical(fs::temp_directory_path());
    options.working_directory = path_to_utf8(directory);
    std::string out = run_capture(options, "", 30.0).out;
    CHECK_EQ(fs::path(out.substr(0, out.size() - 1)).lexically_normal().generic_string(),
             directory.lexically_normal().generic_string());
}

int main(int argc, char** argv) {
    std::vector<std::string> args = utf8_arguments(argc, argv);
    if (args.size() > 1 && args[1] == "--child") return run_child(args);
    return testing::run_all_tests();
}
