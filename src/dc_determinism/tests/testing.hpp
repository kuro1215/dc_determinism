// A minimal unit-test harness (no third-party framework): TEST(name) { CHECK(...); }.
// Each test executable calls `run_all_tests()` from its main(); the exit code is the number of
// failed tests (capped), so CTest sees any failure.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { registry().push_back({name, std::move(body)}); }
};

inline int& failures_in_current_test() {
    static int count = 0;
    return count;
}

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failures_in_current_test();
    std::fprintf(stderr, "  FAILED %s:%d: %s\n", file, line, message.c_str());
}

template <typename T>
std::string show(const T& value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline int run_all_tests() {
    int failed = 0;
    for (const TestCase& test : registry()) {
        failures_in_current_test() = 0;
        std::fprintf(stderr, "[ RUN ] %s\n", test.name.c_str());
        try {
            test.body();
        } catch (const std::exception& error) {
            report_failure(__FILE__, __LINE__, std::string("unexpected exception: ") + error.what());
        }
        if (failures_in_current_test() > 0) {
            ++failed;
            std::fprintf(stderr, "[ FAIL ] %s\n", test.name.c_str());
        } else {
            std::fprintf(stderr, "[  OK  ] %s\n", test.name.c_str());
        }
    }
    std::fprintf(stderr, "%d of %d tests passed\n", static_cast<int>(registry().size()) - failed,
                 static_cast<int>(registry().size()));
    return failed == 0 ? 0 : 1;
}

// True when a `path` key occurs anywhere in `value` (objects and arrays are descended).
template <typename Json>
bool has_path_key(const Json& value) {
    if (value.is_object()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (it.key() == "path" || has_path_key(it.value())) return true;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            if (has_path_key(item)) return true;
        }
    }
    return false;
}
}  // namespace testing

#define TEST(name)                                                                  \
    static void test_##name();                                                      \
    static ::testing::Registrar registrar_##name(#name, test_##name);               \
    static void test_##name()

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) ::testing::report_failure(__FILE__, __LINE__, #condition); \
    } while (0)

#define CHECK_EQ(actual, expected)                                                  \
    do {                                                                            \
        auto&& actual_value = (actual);                                             \
        auto&& expected_value = (expected);                                         \
        if (!(actual_value == expected_value)) {                                    \
            ::testing::report_failure(__FILE__, __LINE__,                           \
                                      std::string(#actual " == " #expected ": got ") + \
                                          ::testing::show(actual_value) + ", expected " + \
                                          ::testing::show(expected_value));         \
        }                                                                           \
    } while (0)

#define CHECK_THROWS(statement)                                                     \
    do {                                                                            \
        bool threw = false;                                                         \
        try {                                                                       \
            statement;                                                              \
        } catch (const std::exception&) {                                           \
            threw = true;                                                           \
        }                                                                           \
        if (!threw) ::testing::report_failure(__FILE__, __LINE__, "expected an exception: " #statement); \
    } while (0)
