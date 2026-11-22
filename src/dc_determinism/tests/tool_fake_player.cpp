// A synthetic stand-in for a player worker used by test_player: `tool_fake_player [options] OUTPUT
// SAMPLES` writes a valid PDET0001 file (256 blocks of SAMPLES records) and a metadata JSON line on
// stdout, and can misbehave on request. Not part of the tool.
//
//   --id N              player id written to the file (1 = normal_dist, 2 = nd2; default 1)
//   --mode M            good (default), alternate (every second call differs), changed (always
//                       differs from `good`), conflict (every second call has another input
//                       configuration), fail (exit 3), garbage, restore-fail, hang
//   --counter FILE      call counter used by `alternate` and `conflict`
//   --log FILE          append `OUTPUT SAMPLES` per call
//   --write-samples N   write N records per block instead of SAMPLES
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

namespace {

void put32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}

std::string pdet(std::uint32_t player, std::uint32_t n, bool changed, bool conflict, bool restore_ok) {
    std::string data = "PDET0001";
    put32(data, player);
    put32(data, 256);
    for (std::uint32_t profile = 0; profile < 4; ++profile) {
        for (std::uint32_t seed = 0; seed < 64; ++seed) {
            put32(data, profile);
            put32(data, seed);
            put32(data, n);
            std::string control(224, '\0');
            if (conflict && profile == 0 && seed == 0) control[0] = 1;
            data += control;
            for (std::uint32_t i = 0; i < n; ++i) {
                put32(data, 1);
                put32(data, 2);
                put32(data, 3);
                put32(data, changed && profile == 0 && seed == 0 && i == 0 ? 0x3f800000u : 0u);
                put32(data, 0);
                put32(data, 0);
            }
            for (std::uint32_t checkpoint : {1u, 2u, 3u, 17u, 624u, 625u}) {
                put32(data, checkpoint);
                put32(data, 1);
                put32(data, restore_ok ? 1u : 0u);
                put32(data, 1);
            }
        }
    }
    return data;
}

}  // namespace

int main(int argc, char** argv) {
    std::uint32_t id = 1;
    std::string mode = "good";
    std::string counter;
    std::string log;
    long write_samples = 0;
    int i = 1;
    for (; i < argc; ++i) {
        std::string argument = argv[i];
        if (argument.compare(0, 2, "--") != 0) break;
        if (i + 1 >= argc) return 64;
        std::string value = argv[++i];
        if (argument == "--id") id = static_cast<std::uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (argument == "--mode") mode = value;
        else if (argument == "--counter") counter = value;
        else if (argument == "--log") log = value;
        else if (argument == "--write-samples") write_samples = std::strtol(value.c_str(), nullptr, 10);
        else return 64;
    }
    if (i + 2 != argc) return 64;
    std::string output = argv[i];
    long samples = std::strtol(argv[i + 1], nullptr, 10);

    if (!log.empty()) {
        std::ofstream stream(log, std::ios::app);
        stream << output << " " << samples << "\n";
    }
    long run = 0;
    if (!counter.empty()) {
        std::ifstream in(counter);
        if (in) in >> run;
        in.close();
        std::ofstream out(counter, std::ios::trunc);
        out << run + 1;
    }
    if (mode == "fail") {
        std::fprintf(stderr, "simulated player failure\n");
        return 3;
    }
    if (mode == "hang") std::this_thread::sleep_for(std::chrono::seconds(600));
    std::string data;
    if (mode == "garbage") {
        data = "garbage";
    } else {
        data = pdet(id, static_cast<std::uint32_t>(write_samples != 0 ? write_samples : samples),
                    mode == "changed" || (mode == "alternate" && run % 2 == 1), mode == "conflict" && run % 2 == 1,
                    mode != "restore-fail");
    }
    std::ofstream stream(output, std::ios::binary);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    std::printf("{\"samples\": %ld, \"compiler\": \"fake\"}\n", 256 * samples);
    return 0;
}
