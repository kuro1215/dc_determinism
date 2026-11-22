#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <vector>

#ifdef PLAYER_ND2
#include "player_nd2.hpp"
using Factory = digitalcurling::players::PlayerND2Factory;
using Storage = digitalcurling::players::PlayerND2Storage;
#else
#include "player_normal_dist.hpp"
using Factory = digitalcurling::players::PlayerNormalDistFactory;
using Storage = digitalcurling::players::PlayerNormalDistStorage;
#endif
#include "xoshiro256plusplus.hpp"

using Shot = digitalcurling::moves::Shot;

static std::uint32_t Bits(float value) {
    std::uint32_t result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

static void Write(std::ostream& output, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) output.put(static_cast<char>((value >> (8 * i)) & 255));
}

static std::array<std::uint32_t, 3> Bits(const Shot& shot) {
    return {Bits(shot.translational_velocity), Bits(shot.angular_velocity), Bits(shot.release_angle)};
}

static Shot Input(std::uint32_t index) {
    constexpr float speeds[] = {0.f, 1.5f, 3.f, 4.f, 5.f, 9.f};
    constexpr float rotations[] = {-2.f, 0.f, 2.f};
    constexpr float angles[] = {0.f, 0.0009765625f, -0.0009765625f, 0.25f, -0.25f};
    return {speeds[index % 6], rotations[index % 3], angles[index % 5]};
}

static Factory Configure(std::uint32_t seed, std::uint32_t profile) {
    Factory factory;
    factory.seed = seed;
    const float speed[] = {0.0076f, 1.f, 0.02f, 0.00000095367431640625f};
    const float angle[] = {0.0018f, 1.f, 0.01f, 0.000000059604644775390625f};
#ifdef PLAYER_ND2
    factory.cw.stddev_speed = factory.ccw.stddev_speed = speed[profile];
    factory.cw.stddev_angle = factory.ccw.stddev_angle = angle[profile];
    factory.cw.stddev_rotation = factory.ccw.stddev_rotation = profile == 0 ? 0.f : speed[profile];
    if (profile == 2) {
        factory.ccw.stddev_speed = 0.03f;
        factory.ccw.stddev_angle = 0.015f;
        factory.ccw.stddev_rotation = 0.1f;
    }
#else
    factory.stddev_speed = speed[profile];
    factory.stddev_angle = angle[profile];
#endif
    return factory;
}

static void Configuration(std::ostream& output, const Factory& factory) {
#ifdef PLAYER_ND2
    for (const auto& p : {factory.cw, factory.ccw}) {
        for (float value : {p.max_speed, p.stddev_speed, p.stddev_rotation, p.stddev_angle})
            Write(output, Bits(value));
    }
#else
    for (int direction = 0; direction < 2; ++direction) {
        for (float value : {factory.max_speed, factory.stddev_speed, 0.f, factory.stddev_angle})
            Write(output, Bits(value));
    }
#endif
}

int main(int argc, char** argv) {
    try {
        static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
        if (argc != 3) throw std::runtime_error("usage: worker OUTPUT SAMPLES_PER_BLOCK");
        const auto count = static_cast<std::uint32_t>(std::stoul(argv[2]));
        if (count < 640 || count > 1000000) throw std::runtime_error("sample count out of range");
        std::ofstream output(argv[1], std::ios::binary);
        output.exceptions(std::ios::failbit | std::ios::badbit);
        output.write("PDET0001", 8);
#ifdef PLAYER_ND2
        Write(output, 2);
#else
        Write(output, 1);
#endif
        Write(output, 256);
        std::vector<std::uint32_t> seeds = {0u, 1u, 2u, 42u, 0x7fffffffu, 0x80000000u, 0xffffffffu};
        for (std::uint32_t i = 1; i <= 57; ++i) seeds.push_back(0x9e3779b9u * i);
        std::uint64_t failures = 0;
        for (std::uint32_t profile = 0; profile < 4; ++profile) {
            for (const auto seed : seeds) {
                auto factory = Configure(seed, profile);
                Write(output, profile);
                Write(output, seed);
                Write(output, count);
                Configuration(output, factory);
                std::mt19937 mt(seed);
                digitalcurling::players::Xoshiro256PlusPlus xo(seed);
                for (int i = 0; i < 16; ++i) Write(output, mt());
                for (int i = 0; i < 16; ++i) {
                    const auto value = xo();
                    Write(output, static_cast<std::uint32_t>(value));
                    Write(output, static_cast<std::uint32_t>(value >> 32));
                }
                auto player = factory.CreatePlayer();
                std::vector<std::array<std::uint32_t, 3>> expected;
                expected.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto input = Input(i);
                    const auto result = player->Play(input);
                    expected.push_back(Bits(result));
                    for (auto value : Bits(input)) Write(output, value);
                    for (auto value : expected.back()) Write(output, value);
                }
                for (std::uint32_t checkpoint : {1u, 2u, 3u, 17u, 624u, 625u}) {
                    auto original = factory.CreatePlayer();
                    for (std::uint32_t i = 0; i < checkpoint; ++i) original->Play(Input(i));
                    auto saved = original->CreateStorage();
                    auto direct = saved->CreatePlayer();
                    auto decoded = saved->ToJson().get<Storage>();
                    auto json = decoded.CreatePlayer();
                    auto loaded = factory.CreatePlayer();
                    loaded->Load(*saved);
                    std::array<bool, 3> equal = {true, true, true};
                    for (std::uint32_t i = checkpoint; i < checkpoint + 8; ++i) {
                        equal[0] = (Bits(direct->Play(Input(i))) == expected[i]) && equal[0];
                        equal[1] = (Bits(json->Play(Input(i))) == expected[i]) && equal[1];
                        equal[2] = (Bits(loaded->Play(Input(i))) == expected[i]) && equal[2];
                    }
                    Write(output, checkpoint);
                    for (const bool value : equal) {
                        Write(output, value ? 1u : 0u);
                        if (!value) ++failures;
                    }
                }
            }
        }
        output.close();
        nlohmann::json metadata = {{"samples", 256ull * count}, {"restore_failures", failures},
                                   {"seed_count", 64}, {"profiles", 4}, {"samples_per_block", count}};
#ifdef _MSC_VER
        metadata["compiler"] = _MSC_VER;
#elif defined(__clang__)
        metadata["compiler"] = __clang_version__;
#else
        metadata["compiler"] = __VERSION__;
#endif
#ifdef _MSVC_STL_VERSION
        metadata["standard_library"] = _MSVC_STL_VERSION;
#elif defined(_LIBCPP_VERSION)
        metadata["standard_library"] = _LIBCPP_VERSION;
#elif defined(__GLIBCXX__)
        metadata["standard_library"] = __GLIBCXX__;
#endif
        std::cout << metadata.dump() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
