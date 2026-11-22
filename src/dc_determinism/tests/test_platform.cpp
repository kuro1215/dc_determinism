// Platform support (windows-amd64, linux-amd64, linux-arm64): the platform keys, the compiler table
// with its `*-nofma` variants, the `--compiler` choices, and the CPU model of an aarch64 /proc/cpuinfo.
// Nothing here needs a prebuilt artifact or a toolchain.
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/spec.hpp"
#include "common/environment.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "localbuild/local_build.hpp"
#include "player/binaries.hpp"
#include "sim/compilers.hpp"
#include "sim/provenance.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

std::string error_of(const std::function<void()>& body) {
    try {
        body();
    } catch (const DcDeterminismError& error) {
        return error.what();
    }
    return std::string();
}

bool parses(const std::string& line) {
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    std::vector<std::string> words;
    std::size_t start = 0;
    while (start < line.size()) {
        std::size_t end = line.find(' ', start);
        if (end == std::string::npos) end = line.size();
        if (end > start) words.push_back(line.substr(start, end - start));
        start = end + 1;
    }
    try {
        parser->parse(words);
        return true;
    } catch (const cli::ParseExit&) {
        return false;
    }
}

}  // namespace

TEST(platform_keys_follow_the_host) {
#if defined(_WIN32)
    const std::string expected = "windows-amd64";
#elif defined(__linux__) && defined(__aarch64__)
    const std::string expected = "linux-arm64";
#else
    const std::string expected = "linux-amd64";
#endif
    CHECK_EQ(std::string(platform_key()), expected);
    CHECK_EQ(host_platform(), expected);
    // The loader and worker locations are derived from the key.
    CHECK(default_worker_path().string().find(expected) != std::string::npos);
    CHECK_EQ(player_binary_key("nd2", "gcc"), expected + "-gcc/nd2");
    CHECK_EQ(player_binary_key("nd2", "wasm"), std::string("wasm/nd2"));
}

TEST(the_compiler_table_lists_the_variants) {
    const std::vector<std::string> expected = {"msvc", "gcc", "clang", "gcc-nofma", "clang-nofma"};
    CHECK(native_compiler_ids() == expected);
    for (const std::string& id : expected) CHECK(is_native_compiler(id));
    CHECK(!is_native_compiler("wasm"));
    CHECK(!is_native_compiler("gcc-nofma2"));

    CHECK_EQ(base_compiler("gcc-nofma"), std::string("gcc"));
    CHECK_EQ(base_compiler("clang-nofma"), std::string("clang"));
    for (const char* id : {"gcc", "clang", "msvc", "wasm"}) CHECK_EQ(base_compiler(id), std::string(id));

    for (const char* id : {"gcc-nofma", "clang-nofma"}) {
        CHECK(is_variant_compiler(id));
        CHECK(extra_compiler_flags(id) == std::vector<std::string>{"-ffp-contract=off"});
    }
    for (const char* id : {"gcc", "clang", "msvc", "wasm"}) {
        CHECK(!is_variant_compiler(id));
        CHECK(extra_compiler_flags(id).empty());
    }
}

TEST(a_nofma_variant_is_available_on_linux_arm64_only) {
    for (const char* id : {"gcc-nofma", "clang-nofma"}) {
        CHECK_EQ(error_of([&] { require_compiler_platform(id, "linux-arm64"); }), std::string());
        for (const char* platform : {"linux-amd64", "windows-amd64"}) {
            CHECK_EQ(error_of([&] { require_compiler_platform(id, platform); }),
                     std::string("--compiler ") + id + " is available on linux-arm64 only");
        }
    }
    for (const char* id : {"gcc", "clang", "msvc"}) {
        for (const char* platform : {"linux-amd64", "windows-amd64", "linux-arm64"}) {
            CHECK_EQ(error_of([&] { require_compiler_platform(id, platform); }), std::string());
        }
    }
}

TEST(the_variants_are_refused_everywhere_they_resolve_a_binary_off_arm64) {
    if (std::string(platform_key()) == "linux-arm64") return;
    const std::string gcc = "--compiler gcc-nofma is available on linux-arm64 only";
    CHECK_EQ(error_of([] { verify_dc_loader("gcc-nofma"); }), gcc);
    CHECK_EQ(error_of([] { loader_provenance("gcc-nofma"); }), gcc);
    CHECK_EQ(error_of([] { player_binary_key("nd2", "gcc-nofma"); }), gcc);
    CHECK_EQ(error_of([] { resolve_player("nd2", "gcc-nofma", ""); }), gcc);
    CHECK_EQ(error_of([] { build_loader("gcc-nofma"); }), gcc);
    CHECK_EQ(error_of([] { build_player("nd2", "gcc-nofma"); }), gcc);
    CHECK_EQ(error_of([] { build_player("nd2", "clang-nofma"); }), "--compiler clang-nofma is available on linux-arm64 only");
}

TEST(the_nofma_ids_are_parser_choices_on_every_platform) {
    for (const char* id : {"gcc-nofma", "clang-nofma"}) {
        std::string compiler = std::string(" --compiler ") + id;
        CHECK(parses("verify" + compiler));
        CHECK(parses("run simulator --prepared p" + compiler + " --engine fcv1 --output r.json"));
        CHECK(parses("benchmark --prepared p" + compiler + " --output b.json"));
        CHECK(parses("capture simulator --prepared p --engine fcv1" + compiler + " --all --output c.zip"));
        CHECK(parses("compare engines --prepared p" + compiler + " --output e.json"));
        CHECK(parses("run player --player nd2" + compiler + " --output o"));
        CHECK(parses("capture player --player nd2" + compiler + " --output o.bin"));
        CHECK(!parses("compare pybind --prepared p --logs-dir l" + compiler + " --output e.json"));
    }
    CHECK(!parses("verify --compiler gcc-nofma2"));
}

TEST(the_unknown_compiler_text_lists_the_ids_of_the_host) {
#if defined(__linux__) && defined(__aarch64__)
    CHECK_EQ(local_compiler_text(false), std::string("gcc, clang, gcc-nofma, clang-nofma"));
    CHECK_EQ(local_compiler_text(true), std::string("gcc, clang, gcc-nofma, clang-nofma, wasm"));
#else
    CHECK_EQ(local_compiler_text(false), std::string("msvc, gcc, clang"));
    CHECK_EQ(local_compiler_text(true), std::string("msvc, gcc, clang, wasm"));
#endif
}

TEST(player_flags_of_a_variant_add_the_contraction_flag_last) {
    for (const char* player : {"nd2", "normal_dist"}) {
        std::vector<std::string> base = player_flags("gcc", player);
        std::vector<std::string> variant = player_flags("gcc-nofma", player);
        CHECK_EQ(variant.size(), base.size() + 1);
        CHECK(std::equal(base.begin(), base.end(), variant.begin()));
        CHECK_EQ(variant.back(), std::string("-ffp-contract=off"));
        CHECK(player_flags("clang-nofma", player).back() == "-ffp-contract=off");
        CHECK(std::find(base.begin(), base.end(), "-ffp-contract=off") == base.end());
    }
}

TEST(cpu_model_of_an_x86_cpuinfo_is_its_model_name) {
    const std::string text =
        "processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Xeon(R) CPU @ 2.20GHz\n\n"
        "processor\t: 1\nmodel name\t: Other\n";
    CHECK_EQ(environment::cpu_model_from_cpuinfo(text).value_or("none"), std::string("Intel(R) Xeon(R) CPU @ 2.20GHz"));
}

TEST(cpu_model_of_an_aarch64_cpuinfo_names_the_implementer_and_part) {
    auto cpuinfo = [](const std::string& implementer, const std::string& part) {
        return "processor\t: 0\nBogoMIPS\t: 48.00\nFeatures\t: fp asimd evtstrm aes pmull sha1 sha2 crc32\n"
               "CPU implementer\t: " + implementer + "\nCPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: " + part +
               "\nCPU revision\t: 1\n\nprocessor\t: 1\nCPU implementer\t: 0x41\nCPU part\t: 0xd40\n";
    };
    CHECK_EQ(environment::cpu_model_from_cpuinfo(cpuinfo("0x61", "0x023")).value_or("none"),
             std::string("Apple CPU implementer 0x61 part 0x023"));
    CHECK_EQ(environment::cpu_model_from_cpuinfo(cpuinfo("0x41", "0xd0c")).value_or("none"),
             std::string("Arm CPU implementer 0x41 part 0xd0c"));
    CHECK_EQ(environment::cpu_model_from_cpuinfo(cpuinfo("0x00", "0x000")).value_or("none"),
             std::string("Emulated/unknown CPU implementer 0x00 part 0x000"));
    CHECK_EQ(environment::cpu_model_from_cpuinfo(cpuinfo("0x4E", "0x0A1")).value_or("none"),
             std::string("CPU implementer 0x4e part 0x0a1"));
    // A "model name" wins when there is one.
    CHECK_EQ(environment::cpu_model_from_cpuinfo(cpuinfo("0x61", "0x023") + "model name\t: X\n").value_or("none"),
             std::string("X"));
}

TEST(cpu_model_is_absent_without_the_fields) {
    CHECK(!environment::cpu_model_from_cpuinfo("").has_value());
    CHECK(!environment::cpu_model_from_cpuinfo("processor\t: 0\nProcessor\t: ARMv8 Processor rev 0 (v8l)\n").has_value());
    CHECK(!environment::cpu_model_from_cpuinfo("CPU implementer\t: 0x61\n").has_value());
}

int main() { return testing::run_all_tests(); }
