// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
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

#include "common.hpp"
#include "loader_test_base.hpp"
#include "plugin_manager_test_access.hpp"

namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins;

class PluginManagerStatic : public ::testing::Test {
protected:
    PluginManager* manager_ = nullptr;
    bool sim_plugin_loaded_ = false;
    bool player_plugin_loaded_ = false;

    void SetUp() override {
        manager_ = &PluginManager::GetInstance();

        if (!(sim_plugin_loaded_ = manager_->IsPluginLoaded(PluginType::simulator, kSimPluginName))) {
            std::cerr << "Simulator plugin '" << kSimPluginName << "' is not statically linked.";
        }
        if (!(player_plugin_loaded_ = manager_->IsPluginLoaded(PluginType::player, kPlayerPluginName))) {
            std::cerr << "Player plugin '" << kPlayerPluginName << "' is not statically linked.";
        }
    }

    void TearDown() override { }

    bool IsSimPluginLoaded() const { return sim_plugin_loaded_; }
    bool IsPlayerPluginLoaded() const { return player_plugin_loaded_; }
};


TEST_F(PluginManagerStatic, StaticPluginsAreLoaded) {
    ASSERT_TRUE(IsSimPluginLoaded());
    ASSERT_TRUE(IsPlayerPluginLoaded());

    EXPECT_TRUE(manager_->IsPluginLoaded(PluginType::simulator, kSimPluginName));
    EXPECT_TRUE(manager_->IsPluginLoaded(PluginType::player, kPlayerPluginName));

    auto sim_plugins = manager_->GetLoadedPlugins(PluginType::simulator);
    auto player_plugins = manager_->GetLoadedPlugins(PluginType::player);

    ASSERT_GE(sim_plugins.size(), 1);
    ASSERT_GE(player_plugins.size(), 1);

    auto sim_it = std::find_if(sim_plugins.begin(), sim_plugins.end(),
        [&](const auto& p) { return p.name == kSimPluginName; });
    auto player_it = std::find_if(player_plugins.begin(), player_plugins.end(),
        [&](const auto& p) { return p.name == kPlayerPluginName; });

    ASSERT_NE(sim_it, sim_plugins.end());
    ASSERT_NE(player_it, player_plugins.end());
}


TEST_F(PluginManagerStatic, Player_Lifecycle) {
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
        ASSERT_EQ(player_target->GetPlayerId(), kPlayerPluginName);
    }

    ASSERT_NE(player_target, nullptr);
}

TEST_F(PluginManagerStatic, Player_SaveAndLoad) {
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

TEST_F(PluginManagerStatic, Player_GetFactory) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }
    auto factory = manager_->CreatePlayerFactory(std::string(kPlayerPluginName));
    auto player = factory->CreatePlayer();

    // Call GetFactory() and check that the returned factory works (Clone)
    auto factory_copy = player->GetFactory();
    ASSERT_NE(factory_copy, nullptr);
    const auto& factory_ref = *factory_copy;
    ASSERT_EQ(factory_ref.GetPlayerId(), kPlayerPluginName);

    std::unique_ptr<players::IPlayerFactory> cloned_factory = nullptr;
    ASSERT_NO_THROW(cloned_factory = factory_ref.Clone());
    ASSERT_NE(cloned_factory, nullptr);
    ASSERT_EQ(cloned_factory->GetPlayerId(), kPlayerPluginName);
}

TEST_F(PluginManagerStatic, PlayerStorage_GetGender) {
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
        if (!manager_->IsPluginLoaded(PluginType::player, name)) {
            std::cerr << "Player plugin '" << name << "' is not statically linked." << std::endl;
            continue;
        }

        auto factory = manager_->CreatePlayerFactory(factory_json);
        ASSERT_NE(factory, nullptr);
        auto player = factory->CreatePlayer();
        ASSERT_NE(player, nullptr);
        auto storage = player->CreateStorage();
        ASSERT_NE(storage, nullptr);

        EXPECT_EQ(storage->GetGender(), players::Gender::kFemale);
    }
}


TEST_F(PluginManagerStatic, Simulator_Lifecycle) {
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

TEST_F(PluginManagerStatic, Simulator_StepAndGetStones) {
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

TEST_F(PluginManagerStatic, Simulator_Simulate) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    auto factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    digitalcurling::test::CheckWrapperSimulate(*factory);
}

TEST_F(PluginManagerStatic, Simulator_Actions_Extended) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    auto factory = manager_->CreateSimulatorFactory(std::string(kSimPluginName));
    auto simulator = digitalcurling::test::ToStepSimulator(factory->CreateSimulator());
    auto* plugin_sim = dynamic_cast<simulators::InvertiblePluginStepSimulator*>(simulator.get());
    ASSERT_NE(plugin_sim, nullptr);

    // 1. Calculate a shot
    moves::Shot shot;
    ASSERT_NO_THROW(shot = plugin_sim->CalculateShot(coordinate::kTee, 0.2f, kHalfPi));
    auto shot_stone = simulators::ISimulator::StoneState{
        {0.f, 0.f},
        0.f,
        shot.ToVector2(),
        shot.angular_velocity
    };

    // 2. Simulate the shot until every stone is at rest
    simulators::ISimulator::AllStones stones;
    stones[0] = shot_stone;
    ASSERT_NO_THROW(plugin_sim->SetStones(stones));
    ASSERT_NO_THROW(plugin_sim->Simulate(coordinate::kSheetWidthMax));
    EXPECT_TRUE(plugin_sim->AreAllStonesStopped());

    // 3. Throw a second stone at the first one and simulate again
    ASSERT_NO_THROW(stones = plugin_sim->GetStones());
    ASSERT_TRUE(stones[0].has_value());
    auto const first_position = stones[0]->position;
    stones[1] = shot_stone;
    ASSERT_NO_THROW(plugin_sim->SetStones(stones));
    ASSERT_NO_THROW(plugin_sim->Simulate(coordinate::kSheetWidthMax));
    EXPECT_TRUE(plugin_sim->AreAllStonesStopped());
    ASSERT_NO_THROW(stones = plugin_sim->GetStones());
    ASSERT_TRUE(stones[0].has_value());
    EXPECT_TRUE(stones[0]->position.x != first_position.x || stones[0]->position.y != first_position.y);

    // 4. GetFactory
    auto factory_copy = simulator->GetFactory();
    ASSERT_NE(factory_copy, nullptr);
    ASSERT_EQ(factory_copy->GetSimulatorId(), kSimPluginName);

    std::unique_ptr<simulators::ISimulatorFactory> cloned_factory = nullptr;
    ASSERT_NO_THROW(cloned_factory = factory_copy->Clone());
    ASSERT_NE(cloned_factory, nullptr);
}

TEST_F(PluginManagerStatic, CreateMissingPlugin_ThrowsPluginNotFound) {
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

TEST_F(PluginManagerStatic, CreateExistingPlugins_StringAndJson) {
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

TEST_F(PluginManagerStatic, ConcurrentLookupAndRegistration) {
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

} // namespace
