// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cstddef>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/loader.h"
#include "digitalcurling/plugins/loader_types.h"
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "digitalcurling/plugins/detail/c_type_converter.hpp"
#include "digitalcurling/players/plugin_player.hpp"
#include "digitalcurling/players/plugin_player_storage.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"

#include "common.hpp"
#include "loader_test_base.hpp"

namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins;

class PluginLoaderStatic : public ::testing::Test {
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


TEST_F(PluginLoaderStatic, StaticPluginsAreLoaded) {
    ASSERT_TRUE(IsSimPluginLoaded());
    ASSERT_TRUE(IsPlayerPluginLoaded());

    bool result;
    ASSERT_EQ(dc_loader_is_plugin_loaded(static_cast<DigitalCurling_PluginType>(PluginType::simulator), kSimPluginName, &result), DIGITALCURLING_OK);
    ASSERT_TRUE(result);
    ASSERT_EQ(dc_loader_is_plugin_loaded(static_cast<DigitalCurling_PluginType>(PluginType::player), kPlayerPluginName, &result), DIGITALCURLING_OK);
    ASSERT_TRUE(result);
}

TEST_F(PluginLoaderStatic, InvalidInstanceErrors) {
    const DigitalCurling_HandleId missing_id = digitalcurling::plugins::HandleId::Generate().value();

    // 2. Try to create a player or simulator from a nonexistent creator ID
    DigitalCurling_HandleId out_id;
    ASSERT_EQ(dc_loader_create_player(missing_id, &out_id), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    ASSERT_EQ(dc_loader_create_simulator(missing_id, &out_id), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    // 3. Try to call Play with a nonexistent player ID
    DigitalCurling_Shot input_shot = { 1.f, 0.f, 0.f };
    DigitalCurling_Shot output_shot;
    ASSERT_EQ(dc_loader_player_play(missing_id, &input_shot, &output_shot), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    // 4. Try to delete a nonexistent instance
    ASSERT_EQ(dc_loader_remove_player_instance(missing_id), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    ASSERT_EQ(dc_loader_remove_simulator_instance(missing_id), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

TEST_F(PluginLoaderStatic, UseAfterRemoveInstance) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id;
    DigitalCurling_HandleId player_id;

    // 1. Create a factory and player
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(factory_id, &player_id), DIGITALCURLING_OK);

    // 2. Delete the player
    ASSERT_EQ(dc_loader_remove_player_instance(player_id), DIGITALCURLING_OK);

    // 3. Call Play with the deleted player ID (fails)
    DigitalCurling_Shot input_shot = { 1.f, 0.f, 0.f };
    DigitalCurling_Shot output_shot;
    ASSERT_EQ(dc_loader_player_play(player_id, &input_shot, &output_shot), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    // 4. Delete the factory
    ASSERT_EQ(dc_loader_remove_player_instance(factory_id), DIGITALCURLING_OK);

    // 5. Try to create a player with the deleted factory ID (fails)
    DigitalCurling_HandleId new_player_id;
    ASSERT_EQ(dc_loader_create_player(factory_id, &new_player_id), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

TEST_F(PluginLoaderStatic, GetErrorMessage) {
    // 1. Intentionally cause an error (NULL argument)
    ASSERT_NE(dc_loader_create_player(0, nullptr), DIGITALCURLING_OK);

    // 2. Get the error message (calculate its size)
    size_t required_size = 0;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    ASSERT_GT(required_size, 0);

    // 3. Get the error message (actual data)
    std::vector<char> buffer(required_size);
    ASSERT_EQ(dc_loader_get_last_error_message(buffer.data(), buffer.size(), &required_size), DIGITALCURLING_OK);

    // 4. Check the contents (the function name should be included)
    std::string message(buffer.data());
    ASSERT_NE(message.find("dc_loader_create_player"), std::string::npos);
}


TEST_F(PluginLoaderStatic, Snapshot_CreatorState) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id;
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory_id), DIGITALCURLING_OK);

    DigitalCurling_HandleId snapshot_id;
    size_t snapshot_size = 0;
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(factory_id, &snapshot_id, &snapshot_size), DIGITALCURLING_OK);
    ASSERT_GT(snapshot_size, 1u);

    std::vector<char> small_buffer(snapshot_size - 1);
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot_id, small_buffer.data(), small_buffer.size()), DIGITALCURLING_ERR_BUFFER_TOO_SMALL);

    std::string json_data = test::GetSnapshotData(snapshot_id, snapshot_size);
    EXPECT_EQ(json_data.size() + 1, snapshot_size);

    char after_destroy[8] = {};
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot_id, after_destroy, sizeof(after_destroy)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    auto j = nlohmann::json::parse(json_data);
    EXPECT_TRUE(j.contains("gender"));

    ASSERT_EQ(dc_loader_remove_player_instance(factory_id), DIGITALCURLING_OK);
}


TEST_F(PluginLoaderStatic, Player_LifecycleAndAction) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id;
    DigitalCurling_HandleId storage_id;
    DigitalCurling_HandleId player_id;

    // 1. Create a factory and storage
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &storage_id), DIGITALCURLING_OK);

    // 2. Create a player from the factory
    ASSERT_EQ(dc_loader_create_player(factory_id, &player_id), DIGITALCURLING_OK);

    // 3. Perform the player action (Play)
    DigitalCurling_Shot input_shot = { 1.f, 0.1f, 0.5f };
    DigitalCurling_Shot output_shot;
    ASSERT_EQ(dc_loader_player_play(player_id, &input_shot, &output_shot), DIGITALCURLING_OK);

    // The "identical" player produces (almost) the same output as its input
    EXPECT_FLOAT_EQ(input_shot.translational_velocity, output_shot.translational_velocity);
    EXPECT_FLOAT_EQ(input_shot.angular_velocity, output_shot.angular_velocity);
    EXPECT_FLOAT_EQ(input_shot.release_angle, output_shot.release_angle);

    // 4. Delete the instance
    ASSERT_EQ(dc_loader_remove_player_instance(player_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(storage_id), DIGITALCURLING_OK);

    // 5. Call Play on the deleted instance (fails)
    ASSERT_EQ(dc_loader_player_play(player_id, &input_shot, &output_shot), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

TEST_F(PluginLoaderStatic, Player_SaveLoad) {
    if (!IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Player plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId storage_id, factory1_id, factory2_id, player1_id, player2_id;
    DigitalCurling_Shot input_shot, output_shot1, output_shot2;
    input_shot = { 3.f, 2.f, 1.f };

    // 1. Create storage
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &storage_id), DIGITALCURLING_OK);

    // 2. Create Player1
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory1_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(factory1_id, &player1_id), DIGITALCURLING_OK);

    // 3. Save Player1 to Storage
    ASSERT_EQ(dc_loader_player_save(player1_id, storage_id), DIGITALCURLING_OK);

    // 4. Play a shot with Player1
    ASSERT_EQ(dc_loader_player_play(player1_id, &input_shot, &output_shot1), DIGITALCURLING_OK);

    // 5. Create Player2
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &factory2_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(factory2_id, &player2_id), DIGITALCURLING_OK);

    // 6. Load Player2 from Storage
    ASSERT_EQ(dc_loader_player_load(player2_id, storage_id), DIGITALCURLING_OK);

    // 7. Play a shot with Player2
    ASSERT_EQ(dc_loader_player_play(player2_id, &input_shot, &output_shot2), DIGITALCURLING_OK);

    // 8. Check that Player1 and Player2 produce the same output
    EXPECT_EQ(output_shot1.translational_velocity, output_shot2.translational_velocity);
    EXPECT_EQ(output_shot1.angular_velocity, output_shot2.angular_velocity);
    EXPECT_EQ(output_shot1.release_angle, output_shot2.release_angle);

    // 7. Clean up
    ASSERT_EQ(dc_loader_remove_player_instance(player1_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(player2_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(factory1_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(factory2_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(storage_id), DIGITALCURLING_OK);
}


TEST_F(PluginLoaderStatic, Simulator_LifecycleAndActions) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id;
    DigitalCurling_HandleId simulator_id;

    // 1. Create a factory and simulator
    const char* json_config = "{\"type\":\"fcv1\", \"seconds_per_frame\":0.01}";
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, json_config, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &simulator_id), DIGITALCURLING_OK);

    // 2. Get the initial stones
    DigitalCurling_StoneCoordinate stones_initial;
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator_id, &stones_initial), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_initial.stones[0].position.x, 0.f);
    EXPECT_FLOAT_EQ(stones_initial.stones[0].position.y, 0.f);
    EXPECT_FLOAT_EQ(stones_initial.stones[0].translational_velocity.y, 0.f);

    // 3. Set stones
    DigitalCurling_StoneCoordinate stones_to_set = {};
    DigitalCurling_StoneState stone_state = {
        {0.f, 10.f}, // position
        0.f, // angle
        {0.f, -1.f}, // translational_velocity
        0.f // angular_velocity
    };
    stones_to_set.stones[0] = stone_state;
    stones_to_set.stones[1] = stone_state;
    stones_to_set.stones[2] = stone_state;
    ASSERT_EQ(dc_loader_simulator_set_stones(simulator_id, &stones_to_set), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_set_stone(simulator_id, 1, nullptr), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_after_remove;
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator_id, &stones_after_remove), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_after_remove.stones[1].position.x, 0.f);
    EXPECT_FLOAT_EQ(stones_after_remove.stones[1].position.y, 0.f);
    EXPECT_FLOAT_EQ(stones_after_remove.stones[1].translational_velocity.x, 0.f);
    EXPECT_FLOAT_EQ(stones_after_remove.stones[1].translational_velocity.y, 0.f);

    DigitalCurling_StoneState updated_stone = {
        {1.f, 11.f},
        1.f,
        {0.25f, 0.5f},
        0.5f
    };
    ASSERT_EQ(dc_loader_simulator_set_stone(simulator_id, 1, &updated_stone), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_set_stone(simulator_id, 2, nullptr), DIGITALCURLING_OK);

    // 4. Get the configured stones (before Step)
    DigitalCurling_StoneCoordinate stones_before_step;
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator_id, &stones_before_step), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_before_step.stones[0].position.y, 10.f);
    EXPECT_FLOAT_EQ(stones_before_step.stones[0].translational_velocity.y, -1.f);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].position.x, updated_stone.position.x);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].position.y, updated_stone.position.y);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].angle, updated_stone.angle);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].translational_velocity.x, updated_stone.translational_velocity.x);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].translational_velocity.y, updated_stone.translational_velocity.y);
    EXPECT_FLOAT_EQ(stones_before_step.stones[1].angular_velocity, updated_stone.angular_velocity);
    EXPECT_FLOAT_EQ(stones_before_step.stones[2].position.x, 0.f);
    EXPECT_FLOAT_EQ(stones_before_step.stones[2].position.y, 0.f);

    // 5. Advance the simulator by one frame
    bool is_stopped_before;
    ASSERT_EQ(dc_loader_simulator_are_all_stones_stopped(simulator_id, &is_stopped_before), DIGITALCURLING_OK);
    ASSERT_FALSE(is_stopped_before);

    ASSERT_EQ(dc_loader_simulator_step(simulator_id, 1, 4.75f), DIGITALCURLING_OK);

    // 6. Get stones after Step
    DigitalCurling_StoneCoordinate stones_after_step;
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator_id, &stones_after_step), DIGITALCURLING_OK);

    // 7. Check whether the state changed
    float dt;
    ASSERT_EQ(dc_loader_simulator_get_seconds_per_frame(simulator_id, &dt), DIGITALCURLING_OK);
    EXPECT_GT(dt, 0.f);
    EXPECT_FLOAT_EQ(dt, 0.001f);
    EXPECT_NEAR(stones_after_step.stones[0].position.x,
        stones_before_step.stones[0].position.x + stones_before_step.stones[0].translational_velocity.x * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step.stones[0].position.y,
        stones_before_step.stones[0].position.y + stones_before_step.stones[0].translational_velocity.y * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step.stones[0].angle,
        stones_before_step.stones[0].angle + stones_before_step.stones[0].angular_velocity * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step.stones[1].position.x,
        stones_before_step.stones[1].position.x + stones_before_step.stones[1].translational_velocity.x * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step.stones[1].position.y,
        stones_before_step.stones[1].position.y + stones_before_step.stones[1].translational_velocity.y * dt,
        1e-5);
    EXPECT_NEAR(stones_after_step.stones[1].angle,
        stones_before_step.stones[1].angle + stones_before_step.stones[1].angular_velocity * dt,
        1e-5);
    EXPECT_FLOAT_EQ(stones_after_step.stones[2].position.x, 0.f);
    EXPECT_FLOAT_EQ(stones_after_step.stones[2].position.y, 0.f);

    // 8. Delete the instance
    ASSERT_EQ(dc_loader_remove_simulator_instance(simulator_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(factory_id), DIGITALCURLING_OK);
}

TEST_F(PluginLoaderStatic, Simulator_SaveLoad) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id, storage_id, sim_id, sim2_id;
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, nullptr, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, nullptr, &storage_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &sim_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &sim2_id), DIGITALCURLING_OK);

    // 1. Set stones in sim_id
    DigitalCurling_StoneCoordinate stones_to_set = {};
    auto& stone_state = stones_to_set.stones[0];
    stone_state = { {0.f, 10.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_EQ(dc_loader_simulator_set_stones(sim_id, &stones_to_set), DIGITALCURLING_OK);

    // 2. Save sim_id to storage
    ASSERT_EQ(dc_loader_simulator_save(sim_id, storage_id), DIGITALCURLING_OK);

    // 3. Load sim2_id from storage
    ASSERT_EQ(dc_loader_simulator_load(sim2_id, storage_id), DIGITALCURLING_OK);

    // 4. Check that sim2_id has the same stones as sim_id
    DigitalCurling_StoneCoordinate stones_loaded;
    ASSERT_EQ(dc_loader_simulator_get_stones(sim2_id, &stones_loaded), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_loaded.stones[0].position.y, stone_state.position.y);
    EXPECT_FLOAT_EQ(stones_loaded.stones[0].translational_velocity.y, stone_state.translational_velocity.y);

    // 5. Clean up
    ASSERT_EQ(dc_loader_remove_simulator_instance(sim_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(sim2_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(storage_id), DIGITALCURLING_OK);
}

TEST_F(PluginLoaderStatic, Simulator_Simulate) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    digitalcurling::test::CheckLoaderSimulate(kSimPluginName);
}

TEST_F(PluginLoaderStatic, Simulator_SimulateMatchesRecordedBaseline) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    digitalcurling::test::CheckLoaderBaseline(kSimPluginName);

    bool fcv1plus_loaded = false;
    ASSERT_EQ(dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_SIMULATOR, "fcv1plus", &fcv1plus_loaded), DIGITALCURLING_OK);
    if (!fcv1plus_loaded) {
        GTEST_SKIP() << "fcv1plus plugin not loaded, skipping its baseline.";
    }
    digitalcurling::test::CheckLoaderBaseline("fcv1plus");
}

} // namespace
