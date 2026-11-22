// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #define NOMINMAX
    #include <windows.h>
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/plugin_error.hpp"
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "digitalcurling/players/plugin_player_factory.hpp"
#include "digitalcurling/players/plugin_player_storage.hpp"
#include "digitalcurling/players/plugin_player.hpp"
#include "digitalcurling/simulators/plugin_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"

#include "dynamic_test_base.hpp"
#include "loader_test_base.hpp"
#include "plugin_manager_test_access.hpp"

namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins;

class PluginManagerDynamic : public digitalcurling::test::DynamicTestBase {
protected:
    PluginManager* manager_ = nullptr;

    void SetUp() override {
        manager_ = &PluginManager::GetInstance();
        digitalcurling::test::DynamicTestBase::SetUp();
    }

    std::optional<std::string> TryLoad(std::filesystem::path const& path, std::string const& expected_name) override {
        if (std::filesystem::exists(path)) {
            try {
                auto loaded = manager_->LoadPlugin(path, true);
                if (!loaded.has_value() || loaded->name != expected_name)
                    return "Loaded plugin name mismatch";
                return std::nullopt;
            } catch (const std::exception& e) {
                return e.what();
            }
        }
        return "file not found";
    }
};


TEST_F(PluginManagerDynamic, LoadPlugins) {
    ASSERT_TRUE(IsSimPluginLoaded()) << "Simulator plugin failed to load in SetUp.";
    ASSERT_TRUE(IsPlayerPluginLoaded()) << "Player plugin failed to load in SetUp.";

    EXPECT_TRUE(manager_->IsPluginLoaded(PluginType::simulator, kSimPluginName));
    EXPECT_TRUE(manager_->IsPluginLoaded(PluginType::player, kPlayerPluginName));

    auto sim_plugins = manager_->GetLoadedPlugins(PluginType::simulator);
    auto player_plugins = manager_->GetLoadedPlugins(PluginType::player);

    EXPECT_EQ(sim_plugins.size(), 1);
    EXPECT_EQ(sim_plugins[0].name, std::string(kSimPluginName));
    EXPECT_EQ(player_plugins.size(), 1);
    EXPECT_EQ(player_plugins[0].name, std::string(kPlayerPluginName));
}

TEST_F(PluginManagerDynamic, LoadNonExistentPlugin) {
    std::filesystem::path bad_path = "non_existent_plugin" + LibraryExtension;
    ASSERT_THROW(manager_->LoadPlugin(bad_path, true), std::runtime_error);
}

TEST_F(PluginManagerDynamic, LoadNonExistentPlugin_ExceptionFalse) {
    std::filesystem::path bad_path = "non_existent_plugin" + LibraryExtension;
    std::optional<PluginId> result;
    ASSERT_NO_THROW(result = manager_->LoadPlugin(bad_path, false));
    ASSERT_FALSE(result.has_value());
}

TEST_F(PluginManagerDynamic, Player_Lifecycle) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    std::unique_ptr<players::IPlayer> player_target;
    {
        std::unique_ptr<players::PluginPlayerFactory> player_factory;
        ASSERT_NO_THROW(player_factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName)));
        ASSERT_NE(player_factory, nullptr);

        ASSERT_NO_THROW(player_target = player_factory->CreatePlayer());
        ASSERT_NE(player_target, nullptr);
        ASSERT_EQ(player_target->GetPlayerId(), std::string(kPlayerPluginName));
    }

    ASSERT_NE(player_target, nullptr);
}

TEST_F(PluginManagerDynamic, Player_SaveAndLoad) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    std::unique_ptr<players::PluginPlayerFactory> factory1, factory2;
    std::unique_ptr<players::IPlayer> player1, player2;

    moves::Shot input_shot{ 3.f, 2.f, 1.f };
    moves::Shot output_shot1, output_shot2;

    // 1. Create storage
    std::unique_ptr<players::PluginPlayerStorage> storage = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));

    // 2. Create Player1
    factory1 = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    player1 = factory1->CreatePlayer();

    // 3. Save Player1 to Storage
    ASSERT_NO_THROW(player1->Save(*storage));

    // 4. Play a shot with Player1
    ASSERT_NO_THROW(output_shot1 = player1->Play(input_shot));

    // 5. Create Player2
    factory2 = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    player2 = factory2->CreatePlayer();

    // 6. Load Player2 from Storage
    ASSERT_NO_THROW(player2->Load(*storage));

    // 7. Play a shot with Player2
    ASSERT_NO_THROW(output_shot2 = player2->Play(input_shot));

    // 8. Check that Player1 and Player2 produce the same output
    EXPECT_EQ(output_shot1.translational_velocity, output_shot2.translational_velocity);
    EXPECT_EQ(output_shot1.angular_velocity, output_shot2.angular_velocity);
    EXPECT_EQ(output_shot1.release_angle, output_shot2.release_angle);
}

TEST_F(PluginManagerDynamic, Player_GetFactory) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }
    auto factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto player = factory->CreatePlayer();

    auto factory_copy = player->GetFactory();
    ASSERT_NE(factory_copy, nullptr);
    const auto& factory_ref = *factory_copy;
    ASSERT_EQ(factory_ref.GetPlayerId(), std::string(kPlayerPluginName));

    std::unique_ptr<players::IPlayerFactory> cloned_factory = nullptr;
    ASSERT_NO_THROW(cloned_factory = factory_ref.Clone());
    ASSERT_NE(cloned_factory, nullptr);
    ASSERT_EQ(cloned_factory->GetPlayerId(), std::string(kPlayerPluginName));
}

TEST_F(PluginManagerDynamic, PlayerStorage_GetGender) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    // identical keeps `gender` at the top level of its state, the others under `factory`
    const std::vector<std::pair<std::string, nlohmann::json>> cases = {
        { "identical", { { "type", "identical" }, { "gender", "female" } } },
        { "normal_dist", {
            { "type", "normal_dist" }, { "gender", "female" },
            { "max_speed", 4.0 }, { "stddev_speed", 0.01 }, { "stddev_angle", 0.01 }
        } },
        { "nd2", {
            { "type", "nd2" }, { "gender", "female" },
            { "cw", { { "max_speed", 4.0 }, { "stddev_speed", 0.01 }, { "stddev_angle", 0.01 }, { "stddev_rotation", 0.01 } } },
            { "ccw", { { "max_speed", 4.0 }, { "stddev_speed", 0.01 }, { "stddev_angle", 0.01 }, { "stddev_rotation", 0.01 } } }
        } }
    };

    for (const auto& [name, factory_json] : cases) {
        SCOPED_TRACE(name);

        // The plugin files of the other players sit next to the nd2 plugin
        const std::filesystem::path nd2_path(player_plugin_path_);
        std::string file_name = nd2_path.filename().string();
        const auto pos = file_name.rfind(kPlayerPluginName);
        if (pos == std::string::npos) {
            GTEST_SKIP() << "Cannot derive the plugin file name for '" << name << "'.";
        }
        file_name.replace(pos, std::string(kPlayerPluginName).size(), name);
        const auto path = nd2_path.parent_path() / file_name;
        if (!std::filesystem::exists(path)) {
            std::cerr << "Player plugin file '" << path.string() << "' not found, skipping." << std::endl;
            continue;
        }

        std::optional<PluginId> loaded;
        ASSERT_NO_THROW(loaded = manager_->LoadPlugin(path, true));
        ASSERT_TRUE(loaded.has_value());
        ASSERT_EQ(loaded->name, name);

        auto factory = manager_->CreatePlayerFactory(factory_json);
        ASSERT_NE(factory, nullptr);
        auto player = factory->CreatePlayer();
        ASSERT_NE(player, nullptr);
        auto storage = player->CreateStorage();
        ASSERT_NE(storage, nullptr);

        EXPECT_EQ(storage->GetGender(), players::Gender::kFemale);
    }
}


TEST_F(PluginManagerDynamic, Simulator_Lifecycle) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    std::unique_ptr<simulators::PluginSimulatorFactory> sim_factory;
    {
        nlohmann::json sim_factory_json = { {"type", std::string(kSimPluginName)}, {"seconds_per_frame", 0.001} };
        ASSERT_NO_THROW(sim_factory = manager_->CreateSimulatorFactory(sim_factory_json));
        ASSERT_NE(sim_factory, nullptr);
        ASSERT_EQ(sim_factory->GetSimulatorId(), std::string(kSimPluginName));
    }

    std::unique_ptr<simulators::PluginSimulatorStorage> sim_storage;
    std::unique_ptr<simulators::ISimulator> sim_target;

    ASSERT_NO_THROW(sim_storage = manager_->CreateSimulatorStorage(std::string(kSimPluginName)));
    ASSERT_NE(sim_storage, nullptr);

    ASSERT_NO_THROW(sim_target = sim_storage->CreateSimulator());
    ASSERT_NE(sim_target, nullptr);
    ASSERT_EQ(sim_target->GetSimulatorId(), std::string(kSimPluginName));
}

TEST_F(PluginManagerDynamic, Simulator_GetFactory) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }
    auto factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    EXPECT_NE(manager_->GetSimulatorCapabilities(kSimPluginName) & DIGITALCURLING_PLUGIN_CAPABILITY_STEP, 0u);
    auto simulator = digitalcurling::test::ToStepSimulator(factory->CreateSimulator());

    auto factory_copy = simulator->GetFactory();
    ASSERT_NE(factory_copy, nullptr);
    const auto& factory_ref = *factory_copy;
    ASSERT_EQ(factory_ref.GetSimulatorId(), std::string(kSimPluginName));

    std::unique_ptr<simulators::ISimulatorFactory> cloned_factory = nullptr;
    ASSERT_NO_THROW(cloned_factory = factory_ref.Clone());
    ASSERT_NE(cloned_factory, nullptr);
    ASSERT_EQ(cloned_factory->GetSimulatorId(), std::string(kSimPluginName));
}

TEST_F(PluginManagerDynamic, Simulator_StepAndGetStones) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    auto factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    auto simulator = digitalcurling::test::ToStepSimulator(factory->CreateSimulator());

    // 1. Get the initial stones
    simulators::ISimulator::AllStones stones_initial;
    ASSERT_NO_THROW(stones_initial = simulator->GetStones());
    ASSERT_FALSE(stones_initial[0].has_value());
    ASSERT_FALSE(stones_initial[1].has_value());
    ASSERT_FALSE(stones_initial[2].has_value());

    // 2. Set stones
    simulators::ISimulator::AllStones stones_to_set;
    simulators::ISimulator::StoneState stone_state {
        {0.f, 10.f}, // position
        0.f, // angle
        {0.f, -1.f}, // translational_velocity
        0.f // angular_velocity
    };
    stones_to_set[0] = stone_state;
    stones_to_set[1] = std::nullopt;
    stones_to_set[2] = stone_state;
    ASSERT_NO_THROW(simulator->SetStones(stones_to_set));

    simulators::ISimulator::AllStones stones_after_set;
    ASSERT_NO_THROW(stones_after_set = simulator->GetStones());
    ASSERT_TRUE(stones_after_set[0].has_value());
    ASSERT_FALSE(stones_after_set[1].has_value());
    ASSERT_TRUE(stones_after_set[2].has_value());

    simulators::ISimulator::StoneState updated_stone {
        {1.f, 11.f},
        1.f,
        {0.25f, 0.5f},
        0.5f
    };
    ASSERT_NO_THROW(simulator->SetStone(StoneIndex(1), updated_stone));
    ASSERT_NO_THROW(simulator->SetStone(StoneIndex(2), std::nullopt));

    // 3. Get the configured stones (before Step)
    auto stones_before_step = simulator->GetStones();
    ASSERT_TRUE(stones_before_step[0].has_value());
    EXPECT_FLOAT_EQ(stones_before_step[0]->position.y, 10.f);
    EXPECT_FLOAT_EQ(stones_before_step[0]->translational_velocity.y, -1.f);
    ASSERT_TRUE(stones_before_step[1].has_value());
    EXPECT_FLOAT_EQ(stones_before_step[1]->position.x, updated_stone.position.x);
    EXPECT_FLOAT_EQ(stones_before_step[1]->position.y, updated_stone.position.y);
    EXPECT_FLOAT_EQ(stones_before_step[1]->angle, updated_stone.angle);
    EXPECT_FLOAT_EQ(stones_before_step[1]->translational_velocity.x, updated_stone.translational_velocity.x);
    EXPECT_FLOAT_EQ(stones_before_step[1]->translational_velocity.y, updated_stone.translational_velocity.y);
    EXPECT_FLOAT_EQ(stones_before_step[1]->angular_velocity, updated_stone.angular_velocity);
    ASSERT_FALSE(stones_before_step[2].has_value());

    // 4. Advance the simulator by one frame
    ASSERT_NO_THROW(simulator->Step());

    // 5. Get stones after Step
    auto stones_after_step = simulator->GetStones();
    float const dt = simulator->GetSecondsPerFrame();
    ASSERT_TRUE(stones_after_step[0].has_value());
    EXPECT_NEAR(stones_after_step[0]->position.x,
        stones_before_step[0]->position.x + stones_before_step[0]->translational_velocity.x * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step[0]->position.y,
        stones_before_step[0]->position.y + stones_before_step[0]->translational_velocity.y * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step[0]->angle,
        stones_before_step[0]->angle + stones_before_step[0]->angular_velocity * dt,
        1e-5);
    ASSERT_TRUE(stones_after_step[1].has_value());
    EXPECT_NEAR(stones_after_step[1]->position.x,
        stones_before_step[1]->position.x + stones_before_step[1]->translational_velocity.x * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step[1]->position.y,
        stones_before_step[1]->position.y + stones_before_step[1]->translational_velocity.y * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step[1]->angle,
        stones_before_step[1]->angle + stones_before_step[1]->angular_velocity * dt,
        1e-5);
    ASSERT_FALSE(stones_after_step[2].has_value());
}

TEST_F(PluginManagerDynamic, Simulator_Simulate) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    auto factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    digitalcurling::test::CheckWrapperSimulate(*factory);
}

TEST_F(PluginManagerDynamic, CreateMissingPlugin_ThrowsPluginNotFound) {
    const std::string missing_name = "non_existent_plugin";
    const nlohmann::json missing_json = { {"type", missing_name} };

    auto expect_not_found = [](auto&& fn) {
        try {
            fn();
            FAIL() << "Expected plugin_error to be thrown.";
        } catch (const plugin_error& e) {
            EXPECT_EQ(e.code(), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
        } catch (...) {
            FAIL() << "Expected plugin_error, but caught a different exception.";
        }
    };

    expect_not_found([&]() { manager_->CreatePlayerFactory(missing_name); });
    expect_not_found([&]() { manager_->CreatePlayerFactory(missing_json); });
    expect_not_found([&]() { manager_->CreatePlayerStorage(missing_name); });
    expect_not_found([&]() { manager_->CreatePlayerStorage(missing_json); });
    expect_not_found([&]() { manager_->CreateSimulatorFactory(missing_name); });
    expect_not_found([&]() { manager_->CreateSimulatorFactory(missing_json); });
    expect_not_found([&]() { manager_->CreateSimulatorStorage(missing_name); });
    expect_not_found([&]() { manager_->CreateSimulatorStorage(missing_json); });
}

TEST_F(PluginManagerDynamic, CreateExistingPlugins_StringAndJson) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto player_factory_str = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    ASSERT_NE(player_factory_str, nullptr);
    EXPECT_EQ(player_factory_str->GetPlayerId(), std::string(kPlayerPluginName));

    auto player_factory_json = manager_->CreatePlayerFactory(player_factory_str->ToJson());
    ASSERT_NE(player_factory_json, nullptr);
    EXPECT_EQ(player_factory_json->GetPlayerId(), std::string(kPlayerPluginName));
    auto player_from_factory = player_factory_json->CreatePlayer();
    ASSERT_NE(player_from_factory, nullptr);
    EXPECT_EQ(player_from_factory->GetPlayerId(), std::string(kPlayerPluginName));

    auto player_storage_str = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
    ASSERT_NE(player_storage_str, nullptr);
    EXPECT_EQ(player_storage_str->GetPlayerId(), std::string(kPlayerPluginName));

    auto player_storage_json = manager_->CreatePlayerStorage(player_storage_str->ToJson());
    ASSERT_NE(player_storage_json, nullptr);
    EXPECT_EQ(player_storage_json->GetPlayerId(), std::string(kPlayerPluginName));
    auto player_from_storage = player_storage_json->CreatePlayer();
    ASSERT_NE(player_from_storage, nullptr);
    EXPECT_EQ(player_from_storage->GetPlayerId(), std::string(kPlayerPluginName));

    auto sim_factory_str = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    ASSERT_NE(sim_factory_str, nullptr);
    EXPECT_EQ(sim_factory_str->GetSimulatorId(), std::string(kSimPluginName));

    auto sim_factory_json = manager_->CreateSimulatorFactory(sim_factory_str->ToJson());
    ASSERT_NE(sim_factory_json, nullptr);
    EXPECT_EQ(sim_factory_json->GetSimulatorId(), std::string(kSimPluginName));
    auto sim_from_factory = digitalcurling::test::ToStepSimulator(sim_factory_json->CreateSimulator());
    ASSERT_NE(sim_from_factory, nullptr);
    EXPECT_EQ(sim_from_factory->GetSimulatorId(), std::string(kSimPluginName));

    auto sim_storage_str = manager_->CreateSimulatorStorage(std::string(kSimPluginName));
    ASSERT_NE(sim_storage_str, nullptr);
    EXPECT_EQ(sim_storage_str->GetSimulatorId(), std::string(kSimPluginName));

    auto sim_storage_json = manager_->CreateSimulatorStorage(sim_storage_str->ToJson());
    ASSERT_NE(sim_storage_json, nullptr);
    EXPECT_EQ(sim_storage_json->GetSimulatorId(), std::string(kSimPluginName));
    auto sim_from_storage = digitalcurling::test::ToStepSimulator(sim_storage_json->CreateSimulator());
    ASSERT_NE(sim_from_storage, nullptr);
    EXPECT_EQ(sim_from_storage->GetSimulatorId(), std::string(kSimPluginName));
}

TEST_F(PluginManagerDynamic, ConcurrentLookupAndRegistration) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto player_resource = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto sim_resource = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(player_resource, nullptr);
    ASSERT_NE(sim_resource, nullptr);

    const auto player_factory_json = manager_->CreatePlayerFactory(std::string(kPlayerPluginName))->ToJson();
    const auto player_storage_json = manager_->CreatePlayerStorage(std::string(kPlayerPluginName))->ToJson();
    const auto sim_factory_json = manager_->CreateSimulatorFactory(std::string(kSimPluginName))->ToJson();
    const auto sim_storage_json = manager_->CreateSimulatorStorage(std::string(kSimPluginName))->ToJson();
    const std::string missing_name = "non_existent_plugin";
    const nlohmann::json missing_json = { {"type", missing_name} };

    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    constexpr int kIterations = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kIterations; ++i) {
                try {
                    manager_->RegisterPlugin(player_resource);
                    manager_->RegisterPlugin(sim_resource);
                } catch (...) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kIterations; ++i) {
                try {
                    if (!manager_->IsPluginLoaded(PluginType::player, kPlayerPluginName) ||
                        !manager_->IsPluginLoaded(PluginType::simulator, kSimPluginName) ||
                        manager_->IsPluginLoaded(PluginType::player, missing_name)) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }

                    auto pf1 = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
                    auto pf2 = manager_->CreatePlayerFactory(player_factory_json);
                    auto ps1 = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
                    auto ps2 = manager_->CreatePlayerStorage(player_storage_json);
                    auto sf1 = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
                    auto sf2 = manager_->CreateSimulatorFactory(sim_factory_json);
                    auto ss1 = manager_->CreateSimulatorStorage(std::string(kSimPluginName));
                    auto ss2 = manager_->CreateSimulatorStorage(sim_storage_json);
                    if (!pf1 || !pf2 || !ps1 || !ps2 || !sf1 || !sf2 || !ss1 || !ss2) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }

                try {
                    manager_->CreatePlayerFactory(missing_name);
                    failures.fetch_add(1, std::memory_order_relaxed);
                } catch (const plugin_error& e) {
                    if (e.code() != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }

                try {
                    manager_->CreateSimulatorStorage(missing_json);
                    failures.fetch_add(1, std::memory_order_relaxed);
                } catch (const plugin_error& e) {
                    if (e.code() != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND) {
                        failures.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(failures.load(), 0);
}

class ScopedPluginCopy {
public:
    ScopedPluginCopy(const std::filesystem::path& source_path, const std::string& tag) {
        if (source_path.empty() || !std::filesystem::exists(source_path)) {
            return;
        }
        const auto parent = source_path.parent_path();
        const auto stem = source_path.stem().string();
        const auto ext = source_path.extension().string();
        copy_path_ = parent / (stem + "_" + tag + ext);

        std::error_code ec;
        std::filesystem::copy_file(
            source_path,
            copy_path_,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (ec) {
            copy_path_.clear();
        }
    }

    ~ScopedPluginCopy() {
        if (!copy_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(copy_path_, ec);
        }
    }

    ScopedPluginCopy(const ScopedPluginCopy&) = delete;
    ScopedPluginCopy& operator=(const ScopedPluginCopy&) = delete;

    const std::filesystem::path& path() const noexcept { return copy_path_; }
    bool valid() const noexcept { return !copy_path_.empty() && std::filesystem::exists(copy_path_); }

private:
    std::filesystem::path copy_path_;
};

class ScopedResourceRestore {
public:
    ScopedResourceRestore(
        PluginManager& manager,
        std::string player_name,
        std::shared_ptr<detail::PlayerPluginResource> player_resource,
        std::string sim_name,
        std::shared_ptr<detail::SimulatorPluginResource> sim_resource
    )
        : manager_(manager),
          player_name_(std::move(player_name)),
          player_resource_(std::move(player_resource)),
          sim_name_(std::move(sim_name)),
          sim_resource_(std::move(sim_resource)) {}

    ~ScopedResourceRestore() {
        detail::PluginManagerTestAccess::SetPlayerResource(manager_, player_name_, player_resource_);
        detail::PluginManagerTestAccess::SetSimulatorResource(manager_, sim_name_, sim_resource_);
    }

    ScopedResourceRestore(const ScopedResourceRestore&) = delete;
    ScopedResourceRestore& operator=(const ScopedResourceRestore&) = delete;

private:
    PluginManager& manager_;
    std::string player_name_;
    std::shared_ptr<detail::PlayerPluginResource> player_resource_;
    std::string sim_name_;
    std::shared_ptr<detail::SimulatorPluginResource> sim_resource_;
};

TEST_F(PluginManagerDynamic, ReloadSameModule_RetainsRegistrationAndPreservesLiveObjects) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto player_res_before = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto sim_res_before = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(player_res_before, nullptr);
    ASSERT_NE(sim_res_before, nullptr);

    // 1. Create live player factory, storage, and instances before reload
    auto player_factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto player_storage = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
    ASSERT_NE(player_factory, nullptr);
    ASSERT_NE(player_storage, nullptr);
    player_factory->SetGender(players::Gender::kFemale);

    auto player_from_factory = player_factory->CreatePlayer();
    ASSERT_NE(player_from_factory, nullptr);
    ASSERT_NO_THROW(player_from_factory->Save(*player_storage));

    auto player_from_storage = player_storage->CreatePlayer();
    ASSERT_NE(player_from_storage, nullptr);

    // 2. Create live simulator factory, storage, and instances before reload
    nlohmann::json sim_config = { {"type", std::string(kSimPluginName)}, {"seconds_per_frame", 0.005} };
    auto sim_factory = manager_->CreateSimulatorFactory(sim_config);
    auto sim_storage = manager_->CreateSimulatorStorage(std::string(kSimPluginName));
    ASSERT_NE(sim_factory, nullptr);
    ASSERT_NE(sim_storage, nullptr);

    auto sim_from_factory = digitalcurling::test::ToStepSimulator(sim_factory->CreateSimulator());
    ASSERT_NE(sim_from_factory, nullptr);

    simulators::ISimulator::AllStones initial_stones{};
    initial_stones[0] = simulators::ISimulator::StoneState{
        {0.f, 10.f},
        0.f,
        {0.f, -1.f},
        0.f
    };
    ASSERT_NO_THROW(sim_from_factory->SetStones(initial_stones));
    ASSERT_NO_THROW(sim_from_factory->Save(*sim_storage));

    auto sim_from_storage = digitalcurling::test::ToStepSimulator(sim_storage->CreateSimulator());
    ASSERT_NE(sim_from_storage, nullptr);

    // 3. Reload the same modules using both canonical and non-canonical paths and both error policies
    const std::filesystem::path player_path(player_plugin_path_);
    const std::filesystem::path sim_path(simulator_plugin_path_);
    const std::filesystem::path player_alt_path = player_path.parent_path() / "." / player_path.filename();
    const std::filesystem::path sim_alt_path = sim_path.parent_path() / "." / sim_path.filename();

    std::optional<PluginId> reload_player_throw;
    std::optional<PluginId> reload_player_nothrow;
    std::optional<PluginId> reload_sim_throw;
    std::optional<PluginId> reload_sim_nothrow;

    ASSERT_NO_THROW(reload_player_throw = manager_->LoadPlugin(player_path, true));
    ASSERT_TRUE(reload_player_throw.has_value());
    EXPECT_EQ(reload_player_throw->type, PluginType::player);
    EXPECT_EQ(reload_player_throw->name, std::string(kPlayerPluginName));

    ASSERT_NO_THROW(reload_player_nothrow = manager_->LoadPlugin(player_alt_path, false));
    ASSERT_TRUE(reload_player_nothrow.has_value());
    EXPECT_EQ(reload_player_nothrow->type, PluginType::player);
    EXPECT_EQ(reload_player_nothrow->name, std::string(kPlayerPluginName));

    ASSERT_NO_THROW(reload_sim_throw = manager_->LoadPlugin(sim_path, true));
    ASSERT_TRUE(reload_sim_throw.has_value());
    EXPECT_EQ(reload_sim_throw->type, PluginType::simulator);
    EXPECT_EQ(reload_sim_throw->name, std::string(kSimPluginName));

    ASSERT_NO_THROW(reload_sim_nothrow = manager_->LoadPlugin(sim_alt_path, false));
    ASSERT_TRUE(reload_sim_nothrow.has_value());
    EXPECT_EQ(reload_sim_nothrow->type, PluginType::simulator);
    EXPECT_EQ(reload_sim_nothrow->name, std::string(kSimPluginName));

    // Verify original registrations were retained
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), player_res_before);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName)), sim_res_before);
    EXPECT_EQ(manager_->GetLoadedPlugins(PluginType::player).size(), 1u);
    EXPECT_EQ(manager_->GetLoadedPlugins(PluginType::simulator).size(), 1u);

    // 4. Exercise live player factory, storage, and instances after reload
    EXPECT_EQ(player_factory->GetGender(), players::Gender::kFemale);
    auto player_factory_json = player_factory->ToJson();
    EXPECT_EQ(player_factory_json.at("type").get<std::string>(), std::string(kPlayerPluginName));

    std::unique_ptr<players::IPlayerFactory> cloned_player_factory;
    ASSERT_NO_THROW(cloned_player_factory = player_factory->Clone());
    ASSERT_NE(cloned_player_factory, nullptr);

    std::unique_ptr<players::IPlayer> post_reload_player_from_factory;
    ASSERT_NO_THROW(post_reload_player_from_factory = player_factory->CreatePlayer());
    ASSERT_NE(post_reload_player_from_factory, nullptr);

    auto player_storage_json = player_storage->ToJson();
    EXPECT_EQ(player_storage_json.at("type").get<std::string>(), std::string(kPlayerPluginName));

    std::unique_ptr<players::IPlayer> post_reload_player_from_storage;
    ASSERT_NO_THROW(post_reload_player_from_storage = player_storage->CreatePlayer());
    ASSERT_NE(post_reload_player_from_storage, nullptr);

    moves::Shot input_shot{ 2.5f, 1.5f, 0.25f };
    moves::Shot shot_out_1{}, shot_out_2{};
    ASSERT_NO_THROW(shot_out_1 = player_from_factory->Play(input_shot));
    ASSERT_NO_THROW(player_from_storage->Load(*player_storage));
    ASSERT_NO_THROW(shot_out_2 = player_from_storage->Play(input_shot));
    EXPECT_FLOAT_EQ(shot_out_1.translational_velocity, shot_out_2.translational_velocity);
    EXPECT_FLOAT_EQ(shot_out_1.angular_velocity, shot_out_2.angular_velocity);
    EXPECT_FLOAT_EQ(shot_out_1.release_angle, shot_out_2.release_angle);
    EXPECT_EQ(player_from_factory->GetGender(), players::Gender::kFemale);

    std::unique_ptr<players::IPlayerStorage> created_player_storage;
    ASSERT_NO_THROW(created_player_storage = player_from_factory->CreateStorage());
    ASSERT_NE(created_player_storage, nullptr);

    // 5. Exercise live simulator factory, storage, and instances after reload
    auto sim_factory_json = sim_factory->ToJson();
    EXPECT_EQ(sim_factory_json.at("type").get<std::string>(), std::string(kSimPluginName));

    std::unique_ptr<simulators::ISimulatorFactory> cloned_sim_factory;
    ASSERT_NO_THROW(cloned_sim_factory = sim_factory->Clone());
    ASSERT_NE(cloned_sim_factory, nullptr);

    std::unique_ptr<simulators::ISimulator> post_reload_sim_from_factory;
    ASSERT_NO_THROW(post_reload_sim_from_factory = sim_factory->CreateSimulator());
    ASSERT_NE(post_reload_sim_from_factory, nullptr);

    auto sim_storage_json = sim_storage->ToJson();
    EXPECT_EQ(sim_storage_json.at("type").get<std::string>(), std::string(kSimPluginName));

    std::unique_ptr<simulators::ISimulator> post_reload_sim_from_storage;
    ASSERT_NO_THROW(post_reload_sim_from_storage = sim_storage->CreateSimulator());
    ASSERT_NE(post_reload_sim_from_storage, nullptr);

    ASSERT_NO_THROW(sim_from_factory->Step());
    ASSERT_NO_THROW(sim_from_storage->Load(*sim_storage));
    ASSERT_NO_THROW(sim_from_storage->Step());

    auto stones_1 = sim_from_factory->GetStones();
    auto stones_2 = sim_from_storage->GetStones();
    ASSERT_TRUE(stones_1[0].has_value());
    ASSERT_TRUE(stones_2[0].has_value());
    EXPECT_FLOAT_EQ(stones_1[0]->position.y, stones_2[0]->position.y);
    EXPECT_FLOAT_EQ(stones_1[0]->translational_velocity.y, stones_2[0]->translational_velocity.y);

    std::unique_ptr<simulators::ISimulatorStorage> created_sim_storage;
    ASSERT_NO_THROW(created_sim_storage = sim_from_factory->CreateStorage());
    ASSERT_NE(created_sim_storage, nullptr);
}

TEST_F(PluginManagerDynamic, ConflictDifferentModule_RejectedAndPreservesLiveObjects) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto player_res_before = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto sim_res_before = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(player_res_before, nullptr);
    ASSERT_NE(sim_res_before, nullptr);

    auto player_factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto player_storage = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
    auto player = player_factory->CreatePlayer();
    ASSERT_NO_THROW(player->Save(*player_storage));

    auto sim_factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    auto sim_storage = manager_->CreateSimulatorStorage(std::string(kSimPluginName));
    auto simulator = digitalcurling::test::ToStepSimulator(sim_factory->CreateSimulator());
    simulators::ISimulator::AllStones stones_to_set{};
    stones_to_set[0] = simulators::ISimulator::StoneState{ {0.f, 9.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_NO_THROW(simulator->SetStones(stones_to_set));
    ASSERT_NO_THROW(simulator->Save(*sim_storage));

    ScopedPluginCopy player_copy(player_plugin_path_, "conflict_mgr_player");
    ScopedPluginCopy sim_copy(simulator_plugin_path_, "conflict_mgr_sim");
    ASSERT_TRUE(player_copy.valid());
    ASSERT_TRUE(sim_copy.valid());

    // 1. Different module file claiming the same player ID must be rejected
    try {
        manager_->LoadPlugin(player_copy.path(), true);
        FAIL() << "Expected plugin_error on conflicting player module load.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    }

    std::optional<PluginId> conflict_player_opt;
    ASSERT_NO_THROW(conflict_player_opt = manager_->LoadPlugin(player_copy.path(), false));
    EXPECT_FALSE(conflict_player_opt.has_value());

    // 2. Different module file claiming the same simulator ID must be rejected
    try {
        manager_->LoadPlugin(sim_copy.path(), true);
        FAIL() << "Expected plugin_error on conflicting simulator module load.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    }

    std::optional<PluginId> conflict_sim_opt;
    ASSERT_NO_THROW(conflict_sim_opt = manager_->LoadPlugin(sim_copy.path(), false));
    EXPECT_FALSE(conflict_sim_opt.has_value());

    // 3. Direct registration of the conflicting modules must also fail.
    auto conflicting_player = detail::CreateTestPluginResource(player_copy.path());
    auto conflicting_simulator = detail::CreateTestPluginResource(sim_copy.path());
    EXPECT_THROW(manager_->RegisterPlugin(conflicting_player), plugin_error);
    EXPECT_THROW(manager_->RegisterPlugin(conflicting_simulator), plugin_error);

    // 4. Original registrations and live objects must remain intact and usable
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), player_res_before);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName)), sim_res_before);

    moves::Shot input_shot{ 2.f, 1.f, 0.5f };
    moves::Shot output_shot{};
    ASSERT_NO_THROW(output_shot = player->Play(input_shot));
    EXPECT_FLOAT_EQ(output_shot.translational_velocity, input_shot.translational_velocity);

    auto player_from_storage = player_storage->CreatePlayer();
    ASSERT_NE(player_from_storage, nullptr);
    ASSERT_NO_THROW(player_from_storage->Load(*player_storage));

    ASSERT_NO_THROW(simulator->Step());
    auto sim_from_storage = digitalcurling::test::ToStepSimulator(sim_storage->CreateSimulator());
    ASSERT_NE(sim_from_storage, nullptr);
    ASSERT_NO_THROW(sim_from_storage->Load(*sim_storage));
    ASSERT_NO_THROW(sim_from_storage->Step());
    EXPECT_FLOAT_EQ(simulator->GetStones()[0]->position.y, sim_from_storage->GetStones()[0]->position.y);

    // Creating new factories and storages from the registration continues to work
    EXPECT_NE(manager_->CreatePlayerFactory(std::string(kPlayerPluginName)), nullptr);
    EXPECT_NE(manager_->CreatePlayerStorage(std::string(kPlayerPluginName)), nullptr);
    EXPECT_NE(manager_->CreateSimulatorFactory(std::string(kSimPluginName)), nullptr);
    EXPECT_NE(manager_->CreateSimulatorStorage(std::string(kSimPluginName)), nullptr);
}

TEST_F(PluginManagerDynamic, BuiltInProtection_RejectsReplacementAndPreservesLiveObjects) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    // 1. Dynamic registration must not be replaced by a built-in resource claiming the same ID
    auto builtin_player_res = detail::CreateTestPluginResource(player_plugin_path_, true);
    auto builtin_sim_res = detail::CreateTestPluginResource(simulator_plugin_path_, true);
    ASSERT_NE(builtin_player_res, nullptr);
    ASSERT_NE(builtin_sim_res, nullptr);
    EXPECT_TRUE(builtin_player_res->IsBuiltIn());
    EXPECT_TRUE(builtin_sim_res->IsBuiltIn());

    EXPECT_THROW(manager_->RegisterPlugin(builtin_player_res), plugin_error);
    EXPECT_THROW(manager_->RegisterPlugin(builtin_sim_res), plugin_error);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), orig_player_res);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName)), orig_sim_res);

    // 2. Built-in registration must never be replaced by dynamic LoadPlugin or another RegisterPlugin
    ScopedResourceRestore restore_guard(
        *manager_,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );

    detail::PluginManagerTestAccess::SetPlayerResource(*manager_, std::string(kPlayerPluginName), nullptr);
    detail::PluginManagerTestAccess::SetSimulatorResource(*manager_, std::string(kSimPluginName), nullptr);
    ASSERT_NO_THROW(manager_->RegisterPlugin(builtin_player_res));
    ASSERT_NO_THROW(manager_->RegisterPlugin(builtin_sim_res));

    // Create live objects from the built-in registrations
    auto builtin_pf = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto builtin_ps = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
    auto builtin_player = builtin_pf->CreatePlayer();
    ASSERT_NO_THROW(builtin_player->Save(*builtin_ps));

    auto builtin_sf = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    auto builtin_ss = manager_->CreateSimulatorStorage(std::string(kSimPluginName));
    auto builtin_sim = digitalcurling::test::ToStepSimulator(builtin_sf->CreateSimulator());
    simulators::ISimulator::AllStones stones{};
    stones[0] = simulators::ISimulator::StoneState{ {0.f, 8.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_NO_THROW(builtin_sim->SetStones(stones));
    ASSERT_NO_THROW(builtin_sim->Save(*builtin_ss));

    // Attempting to load dynamic modules with the same IDs must fail according to error policy
    try {
        manager_->LoadPlugin(player_plugin_path_, true);
        FAIL() << "Expected plugin_error when loading dynamic player over built-in registration.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    }

    std::optional<PluginId> player_opt;
    ASSERT_NO_THROW(player_opt = manager_->LoadPlugin(player_plugin_path_, false));
    EXPECT_FALSE(player_opt.has_value());

    try {
        manager_->LoadPlugin(simulator_plugin_path_, true);
        FAIL() << "Expected plugin_error when loading dynamic simulator over built-in registration.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    }

    std::optional<PluginId> sim_opt;
    ASSERT_NO_THROW(sim_opt = manager_->LoadPlugin(simulator_plugin_path_, false));
    EXPECT_FALSE(sim_opt.has_value());

    // Attempting to register another built-in or dynamic resource over built-in registration must fail
    auto another_builtin_player = detail::CreateTestPluginResource(player_plugin_path_, true);
    auto another_builtin_sim = detail::CreateTestPluginResource(simulator_plugin_path_, true);
    EXPECT_THROW(manager_->RegisterPlugin(another_builtin_player), plugin_error);
    EXPECT_THROW(manager_->RegisterPlugin(another_builtin_sim), plugin_error);
    EXPECT_THROW(manager_->RegisterPlugin(orig_player_res), plugin_error);
    EXPECT_THROW(manager_->RegisterPlugin(orig_sim_res), plugin_error);

    // Re-registering the exact same built-in resource instance remains idempotent
    EXPECT_NO_THROW(manager_->RegisterPlugin(builtin_player_res));
    EXPECT_NO_THROW(manager_->RegisterPlugin(builtin_sim_res));

    // Built-in registrations and their live objects remain intact and usable
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), builtin_player_res);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName)), builtin_sim_res);

    moves::Shot shot_in{ 1.8f, 0.4f, 0.2f };
    moves::Shot shot_out{};
    ASSERT_NO_THROW(shot_out = builtin_player->Play(shot_in));
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);

    auto builtin_player_from_ps = builtin_ps->CreatePlayer();
    ASSERT_NE(builtin_player_from_ps, nullptr);
    ASSERT_NO_THROW(builtin_player_from_ps->Load(*builtin_ps));

    ASSERT_NO_THROW(builtin_sim->Step());
    auto builtin_sim_from_ss = digitalcurling::test::ToStepSimulator(builtin_ss->CreateSimulator());
    ASSERT_NE(builtin_sim_from_ss, nullptr);
    ASSERT_NO_THROW(builtin_sim_from_ss->Load(*builtin_ss));
    ASSERT_NO_THROW(builtin_sim_from_ss->Step());
    EXPECT_FLOAT_EQ(builtin_sim->GetStones()[0]->position.y, builtin_sim_from_ss->GetStones()[0]->position.y);
}

#ifdef _WIN32
bool IsValidUtf8Diagnostic(const std::string& value) {
    return !value.empty() && ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0
    ) > 0;
}
#endif

std::string PathToUtf8String(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

class ScopedTempDir {
public:
    explicit ScopedTempDir(const std::filesystem::path& dir_path) : dir_path_(dir_path) {
        std::error_code ec;
        std::filesystem::remove_all(dir_path_, ec);
        ec.clear();
        std::filesystem::create_directories(dir_path_, ec);
        if (ec) {
            dir_path_.clear();
        }
    }

    ~ScopedTempDir() {
        if (!dir_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(dir_path_, ec);
        }
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const noexcept { return dir_path_; }
    bool valid() const noexcept { return !dir_path_.empty() && std::filesystem::is_directory(dir_path_); }

private:
    std::filesystem::path dir_path_;
};

class ScopedFileCopy {
public:
    ScopedFileCopy(const std::filesystem::path& source_path, const std::filesystem::path& target_path) {
        if (source_path.empty() || target_path.empty() || !std::filesystem::exists(source_path)) {
            return;
        }
        std::error_code ec;
        if (target_path.has_parent_path()) {
            std::filesystem::create_directories(target_path.parent_path(), ec);
            if (ec) return;
        }
        std::filesystem::copy_file(
            source_path,
            target_path,
            std::filesystem::copy_options::overwrite_existing,
            ec
        );
        if (!ec) {
            target_path_ = target_path;
        }
    }

    ~ScopedFileCopy() {
        if (!target_path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(target_path_, ec);
        }
    }

    ScopedFileCopy(const ScopedFileCopy&) = delete;
    ScopedFileCopy& operator=(const ScopedFileCopy&) = delete;

    const std::filesystem::path& path() const noexcept { return target_path_; }
    bool valid() const noexcept { return !target_path_.empty() && std::filesystem::exists(target_path_); }

private:
    std::filesystem::path target_path_;
};

class ScopedCurrentPath {
public:
    explicit ScopedCurrentPath(const std::filesystem::path& target_dir) {
        std::error_code ec;
        original_dir_ = std::filesystem::current_path(ec);
        if (ec) {
            original_dir_.clear();
            return;
        }
        std::filesystem::current_path(target_dir, ec);
        changed_ = !ec;
    }

    ~ScopedCurrentPath() {
        if (!original_dir_.empty()) {
            std::error_code ec;
            std::filesystem::current_path(original_dir_, ec);
        }
    }

    ScopedCurrentPath(const ScopedCurrentPath&) = delete;
    ScopedCurrentPath& operator=(const ScopedCurrentPath&) = delete;

    bool valid() const noexcept { return changed_; }

private:
    std::filesystem::path original_dir_;
    bool changed_ = false;
};

#ifdef _WIN32
class ScopedWindowsModule {
public:
    explicit ScopedWindowsModule(const std::filesystem::path& module_path) {
        if (!module_path.empty()) {
            handle_ = ::LoadLibraryW(module_path.c_str());
        }
    }

    ~ScopedWindowsModule() {
        if (handle_) {
            ::FreeLibrary(handle_);
        }
    }

    ScopedWindowsModule(const ScopedWindowsModule&) = delete;
    ScopedWindowsModule& operator=(const ScopedWindowsModule&) = delete;

    const void* handle() const noexcept { return static_cast<const void*>(handle_); }
    bool valid() const noexcept { return handle_ != nullptr; }

private:
    HMODULE handle_ = nullptr;
};

std::filesystem::path GetWindowsLoadedModulePath(const void* raw_handle) {
    if (!raw_handle) return {};
    std::wstring buffer(MAX_PATH, L'\0');
    while (true) {
        DWORD len = ::GetModuleFileNameW(
             static_cast<HMODULE>(const_cast<void*>(raw_handle)),
             buffer.data(),
             static_cast<DWORD>(buffer.size())
        );
        if (len == 0) return {};
        if (len < buffer.size() - 1) {
            buffer.resize(len);
            return std::filesystem::path(buffer);
        }
        buffer.resize(buffer.size() * 2);
    }
}
#endif

TEST_F(PluginManagerDynamic, BareNameInControlledWorkingDirectory_LoadsRequestedModuleNotSearchPathDecoyAndHandlesNormalizedDuplicates) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    const std::filesystem::path test_bin_dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)).lexically_normal();
    const std::filesystem::path player_bin_dir = std::filesystem::absolute(std::filesystem::path(player_plugin_path_).parent_path()).lexically_normal();
    const std::string bare_name = "digitalcurling_core03_mgr_bare_target" + LibraryExtension;

    ScopedTempDir work_dir(test_bin_dir / "core03_mgr_controlled_cwd");
    ASSERT_TRUE(work_dir.valid());

    // Place a same-name decoy (copy of the simulator plugin) in the executable search path directory
    // and in the plugin directory, and place the requested player plugin in the controlled working directory.
    ScopedFileCopy decoy_in_test_dir(simulator_plugin_path_, test_bin_dir / bare_name);
    ASSERT_TRUE(decoy_in_test_dir.valid());

    std::unique_ptr<ScopedFileCopy> decoy_in_plugin_dir;
    if (player_bin_dir != test_bin_dir) {
        decoy_in_plugin_dir = std::make_unique<ScopedFileCopy>(simulator_plugin_path_, player_bin_dir / bare_name);
        ASSERT_TRUE(decoy_in_plugin_dir->valid());
    }

    ScopedFileCopy requested_in_cwd(player_plugin_path_, work_dir.path() / bare_name);
    ASSERT_TRUE(requested_in_cwd.valid());

#ifdef _WIN32
    // Pre-load the decoy so an unnormalized bare-name LoadLibraryW would match the loaded module list too.
    ScopedWindowsModule preloaded_decoy(decoy_in_test_dir.path());
    ASSERT_TRUE(preloaded_decoy.valid());
#endif

    ScopedCurrentPath cwd_guard(work_dir.path());
    ASSERT_TRUE(cwd_guard.valid());

    ScopedResourceRestore restore_guard(
        *manager_,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );
    detail::PluginManagerTestAccess::SetPlayerResource(*manager_, std::string(kPlayerPluginName), nullptr);
    ASSERT_FALSE(manager_->IsPluginLoaded(PluginType::player, kPlayerPluginName));

    // 1. Load by bare filename from the controlled working directory
    std::optional<PluginId> loaded_id;
    ASSERT_NO_THROW(loaded_id = manager_->LoadPlugin(std::filesystem::path(bare_name), true));
    ASSERT_TRUE(loaded_id.has_value());
    EXPECT_EQ(loaded_id->type, PluginType::player);
    EXPECT_EQ(loaded_id->name, std::string(kPlayerPluginName));

    auto loaded_res = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    ASSERT_NE(loaded_res, nullptr);
    EXPECT_NE(loaded_res, orig_player_res);
    EXPECT_NE(loaded_res->GetModuleHandle(), nullptr);
    EXPECT_NE(loaded_res->GetModuleHandle(), orig_player_res->GetModuleHandle());
    EXPECT_NE(loaded_res->GetModuleHandle(), orig_sim_res->GetModuleHandle());

#ifdef _WIN32
    EXPECT_NE(loaded_res->GetModuleHandle(), preloaded_decoy.handle());
    const auto actual_module_path = GetWindowsLoadedModulePath(loaded_res->GetModuleHandle());
    ASSERT_FALSE(actual_module_path.empty());
    std::error_code eq_ec;
    EXPECT_TRUE(std::filesystem::equivalent(actual_module_path, requested_in_cwd.path(), eq_ec));
    EXPECT_FALSE(eq_ec);
    eq_ec.clear();
    EXPECT_FALSE(std::filesystem::equivalent(actual_module_path, decoy_in_test_dir.path(), eq_ec));
#endif

    // Verify live objects created from the bare-name-loaded module
    auto player_factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto player_storage = manager_->CreatePlayerStorage(std::string(kPlayerPluginName));
    ASSERT_NE(player_factory, nullptr);
    ASSERT_NE(player_storage, nullptr);
    auto player = player_factory->CreatePlayer();
    ASSERT_NE(player, nullptr);
    ASSERT_NO_THROW(player->Save(*player_storage));

    // 2. Ordinary duplicate loading after normalization (relative, dot-segment, and explicit absolute paths)
    const std::filesystem::path dot_rel_path = std::filesystem::path(".") / bare_name;
    const std::filesystem::path parent_rel_path = std::filesystem::path("..") / work_dir.path().filename() / "." / bare_name;
    const std::filesystem::path unnormalized_abs_path = work_dir.path() / "." / ".." / work_dir.path().filename() / bare_name;

    for (const auto& eq_path : { std::filesystem::path(bare_name), dot_rel_path, parent_rel_path, requested_in_cwd.path(), unnormalized_abs_path }) {
        std::optional<PluginId> dup_throw;
        std::optional<PluginId> dup_nothrow;
        ASSERT_NO_THROW(dup_throw = manager_->LoadPlugin(eq_path, true));
        ASSERT_TRUE(dup_throw.has_value());
        EXPECT_EQ(dup_throw->type, PluginType::player);
        EXPECT_EQ(dup_throw->name, std::string(kPlayerPluginName));

        ASSERT_NO_THROW(dup_nothrow = manager_->LoadPlugin(eq_path, false));
        ASSERT_TRUE(dup_nothrow.has_value());
        EXPECT_EQ(dup_nothrow->type, PluginType::player);
        EXPECT_EQ(dup_nothrow->name, std::string(kPlayerPluginName));

        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), loaded_res);
        EXPECT_EQ(manager_->GetLoadedPlugins(PluginType::player).size(), 1u);
    }

    moves::Shot shot_in{ 2.1f, 0.7f, 0.3f };
    moves::Shot shot_out{};
    ASSERT_NO_THROW(shot_out = player->Play(shot_in));
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);

    player.reset();
    player_storage.reset();
    player_factory.reset();
    loaded_res.reset();
}

TEST_F(PluginManagerDynamic, NonAsciiFilesystemPaths_LoadsAndFormatsUtf8Diagnostics) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both player and simulator plugins must be loaded for this test.";
    }

    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(*manager_, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    const std::string non_ascii_dir_utf8 = "core03_mgr_utf8_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xC3\xA4\xC3\xB6\xC3\xBC";
    const std::string non_ascii_file_utf8 = "plugin_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xC3\xA4\xC3\xB6\xC3\xBC" + LibraryExtension;
    const std::string missing_non_ascii_file_utf8 = "missing_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xC3\xA4\xC3\xB6\xC3\xBC" + LibraryExtension;

    const std::filesystem::path base_dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)).lexically_normal();
    ScopedTempDir non_ascii_dir(base_dir / std::filesystem::u8path(non_ascii_dir_utf8));
    ASSERT_TRUE(non_ascii_dir.valid());

    const std::filesystem::path non_ascii_plugin_path = non_ascii_dir.path() / std::filesystem::u8path(non_ascii_file_utf8);
    ScopedFileCopy non_ascii_copy(player_plugin_path_, non_ascii_plugin_path);
    ASSERT_TRUE(non_ascii_copy.valid());

    ScopedCurrentPath cwd_guard(non_ascii_dir.path());
    ASSERT_TRUE(cwd_guard.valid());

    ScopedResourceRestore restore_guard(
        *manager_,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );
    detail::PluginManagerTestAccess::SetPlayerResource(*manager_, std::string(kPlayerPluginName), nullptr);

    // 1. Load via bare non-ASCII std::filesystem::path in non-ASCII working directory
    const std::filesystem::path bare_non_ascii_path = std::filesystem::u8path(non_ascii_file_utf8);
    std::optional<PluginId> loaded_id;
    ASSERT_NO_THROW(loaded_id = manager_->LoadPlugin(bare_non_ascii_path, true));
    ASSERT_TRUE(loaded_id.has_value());
    EXPECT_EQ(loaded_id->type, PluginType::player);
    EXPECT_EQ(loaded_id->name, std::string(kPlayerPluginName));

    auto loaded_res = detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName));
    ASSERT_NE(loaded_res, nullptr);

#ifdef _WIN32
    // Verify wide-character std::filesystem::path construction and duplicate normalization on Windows
    const std::filesystem::path wide_non_ascii_path(non_ascii_plugin_path.wstring());
    std::optional<PluginId> wide_dup_id;
    ASSERT_NO_THROW(wide_dup_id = manager_->LoadPlugin(wide_non_ascii_path, true));
    ASSERT_TRUE(wide_dup_id.has_value());
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), loaded_res);
#endif

    // Duplicate load via unnormalized non-ASCII path under throw_exception = false
    const std::filesystem::path dot_non_ascii_path = non_ascii_dir.path() / "." / std::filesystem::u8path(non_ascii_file_utf8);
    std::optional<PluginId> dup_nothrow;
    ASSERT_NO_THROW(dup_nothrow = manager_->LoadPlugin(dot_non_ascii_path, false));
    ASSERT_TRUE(dup_nothrow.has_value());
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(*manager_, std::string(kPlayerPluginName)), loaded_res);

    auto player_factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    ASSERT_NE(player_factory, nullptr);
    auto player = player_factory->CreatePlayer();
    ASSERT_NE(player, nullptr);
    moves::Shot shot_in{ 1.9f, 0.5f, 0.1f };
    moves::Shot shot_out{};
    ASSERT_NO_THROW(shot_out = player->Play(shot_in));
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);

    // 2. Missing non-ASCII path under both throw_exception policies uses UTF-8 diagnostics
    const std::filesystem::path missing_non_ascii_path = non_ascii_dir.path() / std::filesystem::u8path(missing_non_ascii_file_utf8);
    std::optional<PluginId> missing_nothrow;
    ASSERT_NO_THROW(missing_nothrow = manager_->LoadPlugin(missing_non_ascii_path, false));
    EXPECT_FALSE(missing_nothrow.has_value());

    try {
        manager_->LoadPlugin(missing_non_ascii_path, true);
        FAIL() << "Expected plugin_error for missing non-ASCII path.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
        const std::string what_str = e.what();
#ifdef _WIN32
        EXPECT_TRUE(IsValidUtf8Diagnostic(what_str));
#endif
        EXPECT_NE(what_str.find(non_ascii_dir_utf8), std::string::npos);
        EXPECT_NE(what_str.find(missing_non_ascii_file_utf8), std::string::npos);
    }

    player.reset();
    player_factory.reset();
    loaded_res.reset();
}

TEST_F(PluginManagerDynamic, MissingAndInvalidPaths_RespectThrowExceptionPolicy) {
    const std::filesystem::path base_dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)).lexically_normal();
    ScopedTempDir temp_dir(base_dir / "core03_mgr_invalid_paths");
    ASSERT_TRUE(temp_dir.valid());

    // Directory whose name ends with LibraryExtension (not a regular file)
    ScopedTempDir dir_with_ext(temp_dir.path() / ("directory_not_file" + LibraryExtension));
    ASSERT_TRUE(dir_with_ext.valid());

    // Regular file with wrong extension
    const std::filesystem::path wrong_ext_file = temp_dir.path() / "not_a_plugin.txt";
    {
        std::ofstream ofs(wrong_ext_file, std::ios::binary);
        ofs << "dummy text content";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(wrong_ext_file));

    // Regular file without extension
    const std::filesystem::path no_ext_file = temp_dir.path() / "no_extension_file";
    {
        std::ofstream ofs(no_ext_file, std::ios::binary);
        ofs << "dummy content";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(no_ext_file));

    // Regular file with LibraryExtension but invalid binary contents
    const std::filesystem::path invalid_binary_file = temp_dir.path() / ("invalid_binary" + LibraryExtension);
    {
        std::ofstream ofs(invalid_binary_file, std::ios::binary);
        ofs << "not a valid native shared library binary";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(invalid_binary_file));

    // Path with embedded NUL character
    const std::string nul_str = std::string("embedded_nul\0file", 17) + LibraryExtension;
    const std::filesystem::path embedded_nul_path(nul_str);

    // Path with invalid syntax characters
    const std::filesystem::path invalid_syntax_path = temp_dir.path() / ("bad_chars_<>:?*|" + LibraryExtension);

    const std::vector<std::filesystem::path> invalid_paths = {
        std::filesystem::path{},
        std::filesystem::path("missing_bare_plugin" + LibraryExtension),
        temp_dir.path() / ("missing_abs_plugin" + LibraryExtension),
        temp_dir.path(),
        dir_with_ext.path(),
        wrong_ext_file,
        no_ext_file,
        invalid_binary_file,
        embedded_nul_path,
        invalid_syntax_path
    };

    for (const auto& candidate : invalid_paths) {
        std::optional<PluginId> res_nothrow;
        EXPECT_NO_THROW(res_nothrow = manager_->LoadPlugin(candidate, false))
            << "Unexpected exception under throw_exception=false for path: " << PathToUtf8String(candidate);
        EXPECT_FALSE(res_nothrow.has_value());

        try {
            manager_->LoadPlugin(candidate, true);
            FAIL() << "Expected plugin_error under throw_exception=true for path: " << PathToUtf8String(candidate);
        } catch (const plugin_error& e) {
            EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
            EXPECT_FALSE(std::string(e.what()).empty());
        } catch (...) {
            FAIL() << "Expected plugin_error, but caught a different exception for path: " << PathToUtf8String(candidate);
        }
    }
}

#ifdef _WIN32
TEST_F(PluginManagerDynamic, UnrepresentableFilesystemPath_ReportsValidUtf8) {
    std::wstring invalid = L"invalid_";
    invalid.push_back(static_cast<wchar_t>(0xD800));
    invalid += L".dll";
    const std::filesystem::path path(invalid);
    EXPECT_FALSE(manager_->LoadPlugin(path, false).has_value());
    try {
        manager_->LoadPlugin(path, true);
        FAIL() << "Expected plugin_error.";
    } catch (const plugin_error& e) {
        EXPECT_EQ(e.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
        EXPECT_TRUE(IsValidUtf8Diagnostic(e.what()));
    }
}
#endif

} // namespace
