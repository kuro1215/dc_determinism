// A synthetic stand-in for dc_worker used by test_sim: it speaks the plain request/result line
// protocol (`<position>` in, `<position> stopped <steps> <digest> <digest> <ns>` out) and can
// misbehave on request (`--mode`); it also answers the `O <pos> -` request (result line, then `S <pos> 0 <base64 board>`)
// and the `T <pos>` request (result line, then `T <pos> <bytes>` after writing `<capture-dir>/<pos>.raw`) with the
// synthetic boards of fake_board.hpp (`--truncated 1,3` makes those positions "truncated"); `--version` and `--self-test` answer like the real worker (test_wasm
// runs it in place of node, with the dc_worker.js path as its first argument). Not part of the tool.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "common/sha256.hpp"
#include "fake_board.hpp"

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::printf("dc_worker %s\n", std::string(64, 'a').c_str());
            return 0;
        }
        if (std::string(argv[i]) == "--self-test") {
            std::printf("OK\n");
            return 0;
        }
    }
    std::string mode = "ok";
    std::string capture_dir;
    std::set<long> truncated;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--mode") mode = argv[i + 1];
        if (std::string(argv[i]) == "--capture-dir") capture_dir = argv[i + 1];
        if (std::string(argv[i]) == "--truncated") {
            std::stringstream stream(argv[i + 1]);
            std::string item;
            while (std::getline(stream, item, ',')) {
                if (!item.empty()) truncated.insert(std::strtol(item.c_str(), nullptr, 10));
            }
        }
    }
    if (mode == "startup-fail") {
        std::printf("ERR -1 cannot load loader\n");
        return 2;
    }
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        // `B <position> <engine>`: a benchmark request, answered with `B <position> <engine> <ns>`.
        bool bench = line.compare(0, 2, "B ") == 0;
        bool observed = line.compare(0, 2, "O ") == 0;
        bool captured = line.compare(0, 2, "T ") == 0;
        long position = std::strtol(line.c_str() + (bench || observed || captured ? 2 : 0), nullptr, 10);
        if (mode == "err") {
            std::printf("ERR %ld simulated failure\n", position);
            return 2;
        }
        if (mode == "eof") return 0;
        if (mode == "hang") std::this_thread::sleep_for(std::chrono::seconds(600));
        if (mode == "bad-line") {
            std::printf("garbage\n");
            std::fflush(stdout);
            continue;
        }
        if (bench) {
            std::size_t space = line.find(' ', 2);
            std::string engine = space == std::string::npos ? std::string() : line.substr(space + 1);
            std::printf("B %ld %s %ld\n", position, engine.c_str(), 1000 * (position + 1));
            std::fflush(stdout);
            continue;
        }
        std::string trajectory = dc_determinism::sha256_hex("T" + line);
        std::string final_digest = dc_determinism::sha256_hex("F" + line);
        std::printf("%ld %s %ld %s %s %ld\n", position, truncated.count(position) != 0 ? "truncated" : "stopped",
                    100 + position, trajectory.c_str(), final_digest.c_str(), 1000 + position);
        if (observed) {
            std::string board =
                fakeboard::encode(fakeboard::board(static_cast<int>(position), static_cast<int>(100 + position)));
            std::printf("S %ld 0 %s\n", position, fakeboard::base64(board).c_str());
        }
        if (captured) {
            std::string raw = fakeboard::raw_trajectory(static_cast<int>(position), static_cast<int>(100 + position));
            std::string path = capture_dir + "/" + std::to_string(position) + ".raw";
            {
                std::ofstream stream(path + ".part", std::ios::binary);
                stream.write(raw.data(), static_cast<std::streamsize>(raw.size()));
            }
            std::remove(path.c_str());
            std::rename((path + ".part").c_str(), path.c_str());
            std::printf("T %ld %zu\n", position, raw.size());
        }
        std::fflush(stdout);
    }
    return mode == "exit-3" ? 3 : 0;
}
