// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#define XXH_INLINE_ALL

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xxhash.h>

#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/loader.h"
#include "digitalcurling/plugins/loader_types.h"

#include "common.hpp"
#include "loader_test_base.hpp"

#ifdef DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH
    // Dynamic loading presets: the plugins have to be loaded through the C API first.
    #include "dynamic_test_base.hpp"
#endif

namespace {

using digitalcurling::plugins::HandleId;
using digitalcurling::plugins::detail::HandleIdGenerator;

constexpr std::uint64_t kCounterMask = HandleIdGenerator::kMaxCounter;

std::uint64_t PrefixOf(const HandleId& id) { return id.value() >> HandleIdGenerator::kCounterBits; }
std::uint64_t CounterOf(const HandleId& id) { return id.value() & kCounterMask; }

#ifdef DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH
class HandleIdLoader : public digitalcurling::test::DynamicTestBase {
protected:
    std::optional<std::string> TryLoad(std::filesystem::path const& path, std::string const& expected_name) override {
        if (!std::filesystem::exists(path)) return "file not found";

        const auto path_u8 = path.u8string();
        std::string path_str(path_u8.begin(), path_u8.end());
        DigitalCurling_PluginType type;
        char name_buffer[256];
        const DigitalCurling_ErrorCode err = dc_loader_load_plugin(path_str.c_str(), &type, name_buffer, sizeof(name_buffer));
        if (err != DIGITALCURLING_OK) return "error code " + std::to_string(err);
        if (std::string(name_buffer) != expected_name) return "Loaded plugin name mismatch";
        return std::nullopt;
    }
};
#else
class HandleIdLoader : public ::testing::Test {
protected:
    bool IsPlayerPluginLoaded() const {
        bool loaded = false;
        return dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_PLAYER, kPlayerPluginName, &loaded) == DIGITALCURLING_OK && loaded;
    }
};
#endif

TEST(HandleId, DefaultIsInvalidZero) {
    const HandleId id;
    EXPECT_EQ(id.value(), 0u);
    EXPECT_EQ(id, HandleId::FromValue(0));
}

TEST(HandleId, GenerateNeverReturnsZeroAndIsDistinct) {
    std::unordered_set<HandleId> ids;
    for (int i = 0; i < 10000; ++i) {
        const HandleId id = HandleId::Generate();
        ASSERT_NE(id.value(), 0u);
        ASSERT_TRUE(ids.insert(id).second);
    }
}

TEST(HandleId, GenerateKeepsPrefixAndIncrementsCounter) {
    const HandleId first = HandleId::Generate();
    for (int i = 1; i <= 100; ++i) {
        const HandleId next = HandleId::Generate();
        EXPECT_EQ(PrefixOf(next), PrefixOf(first));
        EXPECT_EQ(CounterOf(next), CounterOf(first) + i);
    }
    EXPECT_GE(CounterOf(first), 1u);
}

TEST(HandleId, GenerateIsDistinctAcrossThreads) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 2000;
    std::vector<std::vector<HandleId>> results(kThreads);

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&results, t, kPerThread]() {
            results[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) results[t].push_back(HandleId::Generate());
        });
    }
    for (auto& thread : threads) thread.join();

    const std::uint64_t prefix = PrefixOf(results[0][0]);
    std::unordered_set<HandleId> ids;
    for (const auto& list : results) {
        std::uint64_t previous = 0;
        for (const auto& id : list) {
            EXPECT_NE(id.value(), 0u);
            EXPECT_EQ(PrefixOf(id), prefix);
            // Each thread sees strictly increasing counters.
            EXPECT_GT(CounterOf(id), previous);
            previous = CounterOf(id);
            ids.insert(id);
        }
    }
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kThreads) * kPerThread);
}

TEST(HandleId, FromValueRoundTrip) {
    for (const std::uint64_t value : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x123456789ABCDEF0}, ~std::uint64_t{0}}) {
        const HandleId id = HandleId::FromValue(value);
        EXPECT_EQ(id.value(), value);
        EXPECT_EQ(id, HandleId::FromValue(value));
    }

    const HandleId generated = HandleId::Generate();
    EXPECT_EQ(HandleId::FromValue(generated.value()), generated);
    EXPECT_NE(HandleId::FromValue(1), HandleId::FromValue(2));
    EXPECT_NE(HandleId::FromValue(1), HandleId::FromValue(std::uint64_t{1} << 63));
}

TEST(HandleId, UsableAsUnorderedMapKey) {
    std::unordered_map<HandleId, int> map;
    const HandleId a = HandleId::Generate();
    const HandleId b = HandleId::Generate();
    map.emplace(a, 1);
    map.emplace(b, 2);

    ASSERT_EQ(map.size(), 2u);
    EXPECT_EQ(map.at(a), 1);
    EXPECT_EQ(map.at(b), 2);
    EXPECT_EQ(std::hash<HandleId>{}(a), std::hash<HandleId>{}(HandleId::FromValue(a.value())));
    EXPECT_EQ(map.erase(a), 1u);
    EXPECT_EQ(map.count(a), 0u);
}

TEST(HandleIdGenerator, UsesGivenPrefixAndStartsAtOne) {
    HandleIdGenerator generator(0xBEEF);
    const HandleId first = generator.Generate();
    const HandleId second = generator.Generate();

    EXPECT_EQ(first.value(), (std::uint64_t{0xBEEF} << 48) | 1u);
    EXPECT_EQ(second.value(), (std::uint64_t{0xBEEF} << 48) | 2u);
}

TEST(HandleIdGenerator, ThrowsAfterLastCounterAndKeepsThrowing) {
    HandleIdGenerator generator(0xABCD, HandleIdGenerator::kMaxCounter - 1);

    EXPECT_EQ(generator.Generate().value(), (std::uint64_t{0xABCD} << 48) | (kCounterMask - 1));
    EXPECT_EQ(generator.Generate().value(), (std::uint64_t{0xABCD} << 48) | kCounterMask);
    for (int i = 0; i < 100; ++i) {
        EXPECT_THROW(generator.Generate(), std::overflow_error);
    }
}

TEST(HandleIdGenerator, ExhaustionIssuesEachRemainingIdOnceAcrossThreads) {
    constexpr int kThreads = 8;
    constexpr int kAttempts = 50;
    constexpr int kRemaining = 5;
    HandleIdGenerator generator(0x1234, HandleIdGenerator::kMaxCounter - kRemaining + 1);

    std::vector<std::vector<HandleId>> issued(kThreads);
    std::atomic<int> thrown{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kAttempts; ++i) {
                try {
                    issued[t].push_back(generator.Generate());
                } catch (const std::overflow_error&) {
                    thrown.fetch_add(1);
                }
            }
        });
    }
    for (auto& thread : threads) thread.join();

    std::unordered_set<HandleId> ids;
    for (const auto& list : issued) ids.insert(list.begin(), list.end());
    EXPECT_EQ(ids.size(), static_cast<std::size_t>(kRemaining));
    EXPECT_EQ(thrown.load(), kThreads * kAttempts - kRemaining);
    for (const auto& id : ids) {
        EXPECT_EQ(PrefixOf(id), 0x1234u);
        EXPECT_GT(CounterOf(id), kCounterMask - kRemaining);
    }
    EXPECT_THROW(generator.Generate(), std::overflow_error);
}

TEST_F(HandleIdLoader, ZeroIdIsNotFound) {
    DigitalCurling_HandleId out = 0;
    size_t size = 0;
    char buffer[16] = {};
    DigitalCurling_Uuid hash{};
    DigitalCurling_Shot shot_in = { 1.f, 0.f, 0.f };
    DigitalCurling_Shot shot_out;

    EXPECT_EQ(dc_loader_snapshot_get_data(0, buffer, sizeof(buffer)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_destroy_snapshot(0), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_creator_get_state_snapshot(0, &out, &size), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_creator_compute_hash(0, &hash), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_player(0, &out), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_simulator(0, &out), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_remove_player_instance(0), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_remove_simulator_instance(0), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_player_play(0, &shot_in, &shot_out), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_simulator_step(0, 1, 4.75f), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

TEST_F(HandleIdLoader, OutputPointerIsStillChecked) {
    EXPECT_EQ(dc_loader_create_player(0, nullptr), DIGITALCURLING_ERR_BUFFER_NULLPTR);
    EXPECT_EQ(dc_loader_get_loaded_plugins_snapshot(DIGITALCURLING_PLUGIN_TYPE_PLAYER, nullptr, nullptr), DIGITALCURLING_ERR_BUFFER_NULLPTR);
    EXPECT_EQ(dc_loader_creator_compute_hash(0, nullptr), DIGITALCURLING_ERR_BUFFER_NULLPTR);
}

TEST_F(HandleIdLoader, DestroyedSnapshotIdIsNotFoundAndFreshIdDiffers) {
    DigitalCurling_HandleId first = 0;
    size_t size = 0;
    ASSERT_EQ(dc_loader_get_loaded_plugins_snapshot(DIGITALCURLING_PLUGIN_TYPE_PLAYER, &first, &size), DIGITALCURLING_OK);
    EXPECT_NE(first, 0u);
    ASSERT_EQ(dc_loader_destroy_snapshot(first), DIGITALCURLING_OK);

    char buffer[256];
    EXPECT_EQ(dc_loader_snapshot_get_data(first, buffer, sizeof(buffer)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_destroy_snapshot(first), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    DigitalCurling_HandleId second = 0;
    ASSERT_EQ(dc_loader_get_loaded_plugins_snapshot(DIGITALCURLING_PLUGIN_TYPE_PLAYER, &second, &size), DIGITALCURLING_OK);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);
    EXPECT_EQ(dc_loader_snapshot_get_data(first, buffer, sizeof(buffer)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_destroy_snapshot(second), DIGITALCURLING_OK);
}

TEST_F(HandleIdLoader, RemovedInstanceIdIsNotFoundAndFreshIdDiffers) {
    if (!IsPlayerPluginLoaded()) GTEST_SKIP() << "Player plugin is not loaded, skipping test.";

    DigitalCurling_HandleId first = 0;
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &first), DIGITALCURLING_OK);
    EXPECT_NE(first, 0u);
    ASSERT_EQ(dc_loader_remove_player_instance(first), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(first), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    DigitalCurling_HandleId second = 0;
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &second), DIGITALCURLING_OK);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);

    DigitalCurling_HandleId player = 0;
    EXPECT_EQ(dc_loader_create_player(first, &player), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_remove_player_instance(second), DIGITALCURLING_OK);
}

TEST_F(HandleIdLoader, ComputeHashIsUuidV8OfXxh3Digest) {
    if (!IsPlayerPluginLoaded()) GTEST_SKIP() << "Player plugin is not loaded, skipping test.";

    DigitalCurling_HandleId factory = 0;
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory), DIGITALCURLING_OK);

    DigitalCurling_HandleId snapshot = 0;
    size_t snapshot_size = 0;
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(factory, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    const std::string state_json = digitalcurling::test::GetSnapshotData(snapshot, snapshot_size);
    ASSERT_FALSE(state_json.empty());

    // XXH3-128 canonical digest of the state JSON with the UUIDv8 version and variant bits applied
    XXH128_canonical_t canonical;
    XXH128_canonicalFromHash(&canonical, XXH3_128bits(state_json.data(), state_json.size()));
    std::uint8_t expected[16];
    std::memcpy(expected, canonical.digest, sizeof(expected));
    expected[6] = (expected[6] & 0x0F) | 0x80;
    expected[8] = (expected[8] & 0x3F) | 0x80;

    DigitalCurling_Uuid hash{};
    ASSERT_EQ(dc_loader_creator_compute_hash(factory, &hash), DIGITALCURLING_OK);
    EXPECT_EQ(std::memcmp(hash.bytes, expected, sizeof(expected)), 0);
    EXPECT_EQ(hash.bytes[6] >> 4, 0x8);
    EXPECT_EQ(hash.bytes[8] & 0xC0, 0x80);

    DigitalCurling_Uuid again{};
    ASSERT_EQ(dc_loader_creator_compute_hash(factory, &again), DIGITALCURLING_OK);
    EXPECT_EQ(std::memcmp(hash.bytes, again.bytes, sizeof(hash.bytes)), 0);

    EXPECT_EQ(dc_loader_remove_player_instance(factory), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_creator_compute_hash(factory, &again), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

} // namespace
