// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #define NOMINMAX
    #include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
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

#include "dynamic_test_base.hpp"
#include "loader_test_base.hpp"
#include "plugin_manager_test_access.hpp"

#ifndef DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH
    #error "DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH is not defined"
#endif


namespace {

using namespace digitalcurling;
using namespace digitalcurling::plugins;

class PluginLoaderDynamic : public digitalcurling::test::DynamicTestBase {
protected:
    std::optional<std::string> TryLoad(std::filesystem::path const& path, std::string const& expected_name) {
        if (std::filesystem::exists(path)) {
            const auto path_u8 = path.u8string();
            std::string path_str(path_u8.begin(), path_u8.end());
            DigitalCurling_PluginType type;
            char name_buffer[256];
            DigitalCurling_ErrorCode err = dc_loader_load_plugin(path_str.c_str(), &type, name_buffer, sizeof(name_buffer));

            if (err != DIGITALCURLING_OK) return "error code " + std::to_string(err);
            else if (std::string(name_buffer) != expected_name) return "Loaded plugin name mismatch";
            return std::nullopt;
        }
        return "file not found";
    }
};


TEST_F(PluginLoaderDynamic, LoadAndUnload) {
    ASSERT_TRUE(IsSimPluginLoaded()) << "Simulator plugin failed to load in SetUp.";
    ASSERT_TRUE(IsPlayerPluginLoaded()) << "Player plugin failed to load in SetUp.";

    bool result;
    ASSERT_EQ(dc_loader_is_plugin_loaded(static_cast<DigitalCurling_PluginType>(PluginType::simulator), kSimPluginName, &result), DIGITALCURLING_OK);
    ASSERT_TRUE(result);
    ASSERT_EQ(dc_loader_is_plugin_loaded(static_cast<DigitalCurling_PluginType>(PluginType::player), kPlayerPluginName, &result), DIGITALCURLING_OK);
    ASSERT_TRUE(result);
}

TEST_F(PluginLoaderDynamic, LoadPluginErrors) {
    // Nonexistent plugin
    DigitalCurling_PluginType type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256];
    std::memset(name_buffer, 'X', sizeof(name_buffer));
    ASSERT_NE(dc_loader_load_plugin("non_existent.dll", &type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_OK);
    EXPECT_EQ(type, DIGITALCURLING_PLUGIN_TYPE_UNKNOWN);
    EXPECT_EQ(name_buffer[0], 'X');

    // NULL arguments (checked in loader.cpp)
    ASSERT_EQ(dc_loader_load_plugin(nullptr, &type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(type, DIGITALCURLING_PLUGIN_TYPE_UNKNOWN);
    EXPECT_EQ(name_buffer[0], 'X');
    ASSERT_EQ(dc_loader_load_plugin(player_plugin_path_.c_str(), nullptr, name_buffer, sizeof(name_buffer)), DIGITALCURLING_ERR_BUFFER_NULLPTR);
    EXPECT_EQ(name_buffer[0], 'X');
    ASSERT_EQ(dc_loader_load_plugin(player_plugin_path_.c_str(), &type, nullptr, sizeof(name_buffer)), DIGITALCURLING_ERR_BUFFER_NULLPTR);
    EXPECT_EQ(type, DIGITALCURLING_PLUGIN_TYPE_UNKNOWN);

    if (IsPlayerPluginLoaded()) {
        DigitalCurling_PluginType small_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
        char small_name_buffer[2] = { 'X', 'Y' }; // Obviously too small a buffer
        DigitalCurling_ErrorCode err = dc_loader_load_plugin(player_plugin_path_.c_str(), &small_type, small_name_buffer, sizeof(small_name_buffer));
        ASSERT_EQ(err, DIGITALCURLING_ERR_BUFFER_TOO_SMALL);
        EXPECT_EQ(small_type, DIGITALCURLING_PLUGIN_TYPE_UNKNOWN);
        EXPECT_EQ(small_name_buffer[0], 'X');
        EXPECT_EQ(small_name_buffer[1], 'Y');
    }
}

TEST_F(PluginLoaderDynamic, InvalidInstanceErrors) {
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

TEST_F(PluginLoaderDynamic, UseAfterRemoveInstance) {
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


TEST_F(PluginLoaderDynamic, GetErrorMessage) {
    // 1. Cause an error (NULL argument)
    ASSERT_NE(dc_loader_load_plugin(nullptr, nullptr, nullptr, 0), DIGITALCURLING_OK);

    // 2. Get the error message (calculate its size)
    size_t required_size = 0;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    ASSERT_GT(required_size, 0);

    // 3. Get the error message (actual data)
    std::vector<char> buffer(required_size);
    ASSERT_EQ(dc_loader_get_last_error_message(buffer.data(), buffer.size(), &required_size), DIGITALCURLING_OK);

    // 4. Check the contents (the function name should be included)
    std::string message(buffer.data());
    ASSERT_NE(message.find("dc_loader_load_plugin"), std::string::npos);
}

TEST_F(PluginLoaderDynamic, ErrorMessageIsClearedBySuccessfulCall) {
    // 1. Cause an error, then a successful call must clear the message
    ASSERT_NE(dc_loader_load_plugin(nullptr, nullptr, nullptr, 0), DIGITALCURLING_OK);
    size_t required_size = 0;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    ASSERT_GT(required_size, 1u);

    bool loaded = true;
    ASSERT_EQ(dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_PLAYER, "no-such-plugin", &loaded), DIGITALCURLING_OK);
    EXPECT_FALSE(loaded);
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    EXPECT_EQ(required_size, 1u);

    // 2. Reading the message must not clear it
    ASSERT_NE(dc_loader_load_plugin(nullptr, nullptr, nullptr, 0), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    const size_t first_size = required_size;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    EXPECT_EQ(required_size, first_size);
    EXPECT_GT(first_size, 1u);
}

TEST_F(PluginLoaderDynamic, ErrorMessageBufferTooSmall) {
    ASSERT_NE(dc_loader_load_plugin(nullptr, nullptr, nullptr, 0), DIGITALCURLING_OK);
    size_t required_size = 0;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &required_size), DIGITALCURLING_OK);
    ASSERT_GT(required_size, 1u);

    std::vector<char> buffer(required_size, 'X');
    EXPECT_EQ(dc_loader_get_last_error_message(buffer.data(), required_size - 1, &required_size), DIGITALCURLING_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(buffer[0], 'X');
    EXPECT_EQ(dc_loader_get_last_error_message(buffer.data(), buffer.size(), &required_size), DIGITALCURLING_OK);
}


TEST_F(PluginLoaderDynamic, Snapshot_LoadedPlugins) {
    if (!IsSimPluginLoaded() || !IsPlayerPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    DigitalCurling_HandleId snapshot_id;
    size_t snapshot_size = 0;
    const char separator = ';';

    // 1. Get a snapshot of player plugins
    ASSERT_EQ(dc_loader_get_loaded_plugins_snapshot(DIGITALCURLING_PLUGIN_TYPE_PLAYER, &snapshot_id, &snapshot_size), DIGITALCURLING_OK);
    std::string player_data = test::GetSnapshotData(snapshot_id, snapshot_size);
    EXPECT_EQ(player_data.size() + 1, snapshot_size);
    EXPECT_EQ(player_data, "[\"" + std::string(kPlayerPluginName) + "\"]");

    // 2. Get a snapshot of simulator plugins
    ASSERT_EQ(dc_loader_get_loaded_plugins_snapshot(DIGITALCURLING_PLUGIN_TYPE_SIMULATOR, &snapshot_id, &snapshot_size), DIGITALCURLING_OK);
    std::string sim_data = test::GetSnapshotData(snapshot_id, snapshot_size);
    EXPECT_EQ(sim_data.size() + 1, snapshot_size);
    EXPECT_EQ(sim_data, "[\"" + std::string(kSimPluginName) + "\"]");
}

TEST_F(PluginLoaderDynamic, Snapshot_CreatorState) {
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
    ASSERT_FALSE(json_data.empty());
    ASSERT_EQ(json_data.size() + 1, snapshot_size);

    char after_destroy[8] = {};
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot_id, after_destroy, sizeof(after_destroy)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);

    auto j = nlohmann::json::parse(json_data);
    ASSERT_TRUE(j.contains("gender"));

    ASSERT_EQ(dc_loader_remove_player_instance(factory_id), DIGITALCURLING_OK);
}


TEST_F(PluginLoaderDynamic, Player_LifecycleAndAction) {
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
    DigitalCurling_Shot input_shot = { 1.f, 0.1f, 0.5f }; // speed, angle, rotation
    DigitalCurling_Shot output_shot;
    ASSERT_EQ(dc_loader_player_play(player_id, &input_shot, &output_shot), DIGITALCURLING_OK);

    // The default ND2 parameters leave this shot unchanged
    EXPECT_FLOAT_EQ(input_shot.translational_velocity, output_shot.translational_velocity);
    EXPECT_FLOAT_EQ(input_shot.angular_velocity, output_shot.angular_velocity);
    EXPECT_FLOAT_EQ(input_shot.release_angle, output_shot.release_angle);

    // 4. Delete the instance (loader.cpp implementation)
    ASSERT_EQ(dc_loader_remove_player_instance(player_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(storage_id), DIGITALCURLING_OK);

    // 5. Call Play on the deleted instance (should fail)
    ASSERT_EQ(dc_loader_player_play(player_id, &input_shot, &output_shot), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
}

TEST_F(PluginLoaderDynamic, Player_SaveLoad) {
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


TEST_F(PluginLoaderDynamic, Simulator_LifecycleAndActions) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    DigitalCurling_HandleId factory_id;
    DigitalCurling_HandleId simulator_id;

    // 1. Create a factory and simulator
    const char* json_config = "{\"type\":\"fcv1\", \"seconds_per_frame\":0.01}";
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, json_config, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &simulator_id), DIGITALCURLING_OK);
    DigitalCurling_PluginCapabilities capabilities = 0;
    ASSERT_EQ(dc_loader_simulator_get_capabilities(simulator_id, &capabilities), DIGITALCURLING_OK);
    EXPECT_NE(capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_STEP, 0u);
    EXPECT_EQ(capabilities & 1u, 0u);

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
    ASSERT_FALSE(is_stopped_before); // Should be moving

    ASSERT_EQ(dc_loader_simulator_step(simulator_id, 1, 0), DIGITALCURLING_OK);

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

TEST_F(PluginLoaderDynamic, Simulator_SaveLoad) {
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

TEST_F(PluginLoaderDynamic, Simulator_Simulate) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    digitalcurling::test::CheckLoaderSimulate(kSimPluginName);
}

TEST_F(PluginLoaderDynamic, Simulator_SimulateMatchesRecordedBaseline) {
    if (!IsSimPluginLoaded()) {
        GTEST_SKIP() << "Simulator plugin not loaded, skipping test.";
    }

    // Only fcv1 is loaded here: loading further plugins would change the state that other tests of this
    // process expect. The fcv1plus plugin is checked through the C API by the bundled (static) loader tests.
    digitalcurling::test::CheckLoaderBaseline(kSimPluginName);
}

TEST_F(PluginLoaderDynamic, CreateMissingPluginErrors) {
    DigitalCurling_HandleId out_id;
    const char* missing_name = "non_existent_plugin";
    const char* missing_json = "{\"type\":\"non_existent_plugin\"}";

    EXPECT_EQ(dc_loader_create_player_factory(missing_name, nullptr, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_player_factory(missing_name, missing_json, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_player_storage(missing_name, nullptr, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_player_storage(missing_name, missing_json, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_simulator_factory(missing_name, nullptr, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_simulator_factory(missing_name, missing_json, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_simulator_storage(missing_name, nullptr, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
    EXPECT_EQ(dc_loader_create_simulator_storage(missing_name, missing_json, &out_id), DIGITALCURLING_ERR_PLUGIN_NOT_FOUND);
}

TEST_F(PluginLoaderDynamic, CreateExistingPlugins_NullAndJson) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto get_creator_json = [](const DigitalCurling_HandleId& creator_id) -> std::string {
        DigitalCurling_HandleId handle{};
        size_t snapshot_size = 0;
        if (dc_loader_creator_get_state_snapshot(creator_id, &handle, &snapshot_size) != DIGITALCURLING_OK) {
            return {};
        }
        return test::GetSnapshotData(handle, snapshot_size);
    };

    DigitalCurling_HandleId pf_null, pf_json, ps_null, ps_json, p1, p2;
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_null), DIGITALCURLING_OK);
    std::string pf_state = get_creator_json(pf_null);
    ASSERT_FALSE(pf_state.empty());
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, pf_state.c_str(), &pf_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_json, &p1), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &ps_null), DIGITALCURLING_OK);
    std::string ps_state = get_creator_json(ps_null);
    ASSERT_FALSE(ps_state.empty());
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, ps_state.c_str(), &ps_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(ps_json, &p2), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_remove_player_instance(p1), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(p2), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(pf_null), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(pf_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(ps_null), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_player_instance(ps_json), DIGITALCURLING_OK);

    DigitalCurling_HandleId sf_null, sf_json, ss_null, ss_json, s1, s2;
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, nullptr, &sf_null), DIGITALCURLING_OK);
    std::string sf_state = get_creator_json(sf_null);
    ASSERT_FALSE(sf_state.empty());
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, sf_state.c_str(), &sf_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(sf_json, &s1), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, nullptr, &ss_null), DIGITALCURLING_OK);
    std::string ss_state = get_creator_json(ss_null);
    ASSERT_FALSE(ss_state.empty());
    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, ss_state.c_str(), &ss_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(ss_json, &s2), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_remove_simulator_instance(s1), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(s2), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(sf_null), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(sf_json), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(ss_null), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(ss_json), DIGITALCURLING_OK);
}

TEST_F(PluginLoaderDynamic, ConcurrentLookupAndRegistration) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto player_resource = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto sim_resource = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(player_resource, nullptr);
    ASSERT_NE(sim_resource, nullptr);

    const std::string pf_json = manager.CreatePlayerFactory(std::string(kPlayerPluginName))->ToJson().dump();
    const std::string ps_json = manager.CreatePlayerStorage(std::string(kPlayerPluginName))->ToJson().dump();
    const std::string sf_json = manager.CreateSimulatorFactory(std::string(kSimPluginName))->ToJson().dump();
    const std::string ss_json = manager.CreateSimulatorStorage(std::string(kSimPluginName))->ToJson().dump();
    const char* missing_name = "non_existent_plugin";
    const char* missing_json = "{\"type\":\"non_existent_plugin\"}";

    std::atomic<bool> start{false};
    std::atomic<int> failures{0};
    constexpr int kIterations = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {}
            for (int i = 0; i < kIterations; ++i) {
                try {
                    manager.RegisterPlugin(player_resource);
                    manager.RegisterPlugin(sim_resource);
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
                bool loaded = false;
                if (dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_PLAYER, kPlayerPluginName, &loaded) != DIGITALCURLING_OK || !loaded) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                if (dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_SIMULATOR, kSimPluginName, &loaded) != DIGITALCURLING_OK || !loaded) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }

                DigitalCurling_HandleId id1{}, id2{}, id3{}, id4{}, id5{}, id6{}, id7{}, id8{};
                if (dc_loader_create_player_factory(kPlayerPluginName, nullptr, &id1) != DIGITALCURLING_OK ||
                    dc_loader_create_player_factory(kPlayerPluginName, pf_json.c_str(), &id2) != DIGITALCURLING_OK ||
                    dc_loader_create_player_storage(kPlayerPluginName, nullptr, &id3) != DIGITALCURLING_OK ||
                    dc_loader_create_player_storage(kPlayerPluginName, ps_json.c_str(), &id4) != DIGITALCURLING_OK ||
                    dc_loader_create_simulator_factory(kSimPluginName, nullptr, &id5) != DIGITALCURLING_OK ||
                    dc_loader_create_simulator_factory(kSimPluginName, sf_json.c_str(), &id6) != DIGITALCURLING_OK ||
                    dc_loader_create_simulator_storage(kSimPluginName, nullptr, &id7) != DIGITALCURLING_OK ||
                    dc_loader_create_simulator_storage(kSimPluginName, ss_json.c_str(), &id8) != DIGITALCURLING_OK) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
                dc_loader_remove_player_instance(id1);
                dc_loader_remove_player_instance(id2);
                dc_loader_remove_player_instance(id3);
                dc_loader_remove_player_instance(id4);
                dc_loader_remove_simulator_instance(id5);
                dc_loader_remove_simulator_instance(id6);
                dc_loader_remove_simulator_instance(id7);
                dc_loader_remove_simulator_instance(id8);

                DigitalCurling_HandleId missing_id;
                if (dc_loader_create_player_factory(missing_name, nullptr, &missing_id) != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND ||
                    dc_loader_create_player_storage(missing_name, missing_json, &missing_id) != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND ||
                    dc_loader_create_simulator_factory(missing_name, nullptr, &missing_id) != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND ||
                    dc_loader_create_simulator_storage(missing_name, missing_json, &missing_id) != DIGITALCURLING_ERR_PLUGIN_NOT_FOUND) {
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

TEST_F(PluginLoaderDynamic, ReloadSameModule_PreservesLivePlayerAndSimulatorInstances) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto player_res_before = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto sim_res_before = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(player_res_before, nullptr);
    ASSERT_NE(sim_res_before, nullptr);

    // 1. Create live player factory, storage, and instances before reload
    DigitalCurling_HandleId pf_id{}, ps_id{}, p_from_pf_id{}, p_from_ps_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_from_pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_save(p_from_pf_id, ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(ps_id, &p_from_ps_id), DIGITALCURLING_OK);

    // 2. Create live simulator factory, storage, and instances before reload
    DigitalCurling_HandleId sf_id{}, ss_id{}, s_from_sf_id{}, s_from_ss_id{};
    const char* sim_config = "{\"type\":\"fcv1\", \"seconds_per_frame\":0.01}";
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, sim_config, &sf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, nullptr, &ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(sf_id, &s_from_sf_id), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_to_set = {};
    stones_to_set.stones[0] = { {0.f, 10.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_EQ(dc_loader_simulator_set_stones(s_from_sf_id, &stones_to_set), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_save(s_from_sf_id, ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(ss_id, &s_from_ss_id), DIGITALCURLING_OK);

    // 3. Reload the same modules through C API using canonical and non-canonical paths
    const std::filesystem::path player_path(player_plugin_path_);
    const std::filesystem::path sim_path(simulator_plugin_path_);
    const std::string player_alt_path = (player_path.parent_path() / "." / player_path.filename()).string();
    const std::string sim_alt_path = (sim_path.parent_path() / "." / sim_path.filename()).string();

    DigitalCurling_PluginType loaded_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256] = {};

    ASSERT_EQ(dc_loader_load_plugin(player_plugin_path_.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_OK);
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
    EXPECT_STREQ(name_buffer, kPlayerPluginName);

    ASSERT_EQ(dc_loader_load_plugin(player_alt_path.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_OK);
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
    EXPECT_STREQ(name_buffer, kPlayerPluginName);

    ASSERT_EQ(dc_loader_load_plugin(simulator_plugin_path_.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_OK);
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_SIMULATOR);
    EXPECT_STREQ(name_buffer, kSimPluginName);

    ASSERT_EQ(dc_loader_load_plugin(sim_alt_path.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)), DIGITALCURLING_OK);
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_SIMULATOR);
    EXPECT_STREQ(name_buffer, kSimPluginName);

    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), player_res_before);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName)), sim_res_before);

    // 4. Exercise live player factory, storage, and instances after reload
    DigitalCurling_HandleId snapshot{};
    size_t snapshot_size = 0;
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(pf_id, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    EXPECT_FALSE(test::GetSnapshotData(snapshot, snapshot_size).empty());
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(ps_id, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    EXPECT_FALSE(test::GetSnapshotData(snapshot, snapshot_size).empty());

    DigitalCurling_Uuid creator_hash{};
    EXPECT_EQ(dc_loader_creator_compute_hash(pf_id, &creator_hash), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_creator_compute_hash(ps_id, &creator_hash), DIGITALCURLING_OK);

    DigitalCurling_HandleId post_reload_p_from_pf{}, post_reload_p_from_ps{};
    ASSERT_EQ(dc_loader_create_player(pf_id, &post_reload_p_from_pf), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(ps_id, &post_reload_p_from_ps), DIGITALCURLING_OK);

    DigitalCurling_Shot input_shot = { 2.5f, 1.2f, 0.3f };
    DigitalCurling_Shot out_shot_1 = {}, out_shot_2 = {};
    DigitalCurling_Gender gender = DIGITALCURLING_GENDER_UNKNOWN;
    ASSERT_EQ(dc_loader_player_get_gender(p_from_pf_id, &gender), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_play(p_from_pf_id, &input_shot, &out_shot_1), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_load(p_from_ps_id, ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_play(p_from_ps_id, &input_shot, &out_shot_2), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(out_shot_1.translational_velocity, out_shot_2.translational_velocity);
    EXPECT_FLOAT_EQ(out_shot_1.angular_velocity, out_shot_2.angular_velocity);
    EXPECT_FLOAT_EQ(out_shot_1.release_angle, out_shot_2.release_angle);

    // 5. Exercise live simulator factory, storage, and instances after reload
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(sf_id, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    EXPECT_FALSE(test::GetSnapshotData(snapshot, snapshot_size).empty());
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(ss_id, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    EXPECT_FALSE(test::GetSnapshotData(snapshot, snapshot_size).empty());
    EXPECT_EQ(dc_loader_creator_compute_hash(sf_id, &creator_hash), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_creator_compute_hash(ss_id, &creator_hash), DIGITALCURLING_OK);

    DigitalCurling_HandleId post_reload_s_from_sf{}, post_reload_s_from_ss{};
    ASSERT_EQ(dc_loader_create_simulator(sf_id, &post_reload_s_from_sf), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(ss_id, &post_reload_s_from_ss), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_simulator_step(s_from_sf_id, 1, 0.f), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_load(s_from_ss_id, ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_step(s_from_ss_id, 1, 0.f), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_1 = {}, stones_2 = {};
    ASSERT_EQ(dc_loader_simulator_get_stones(s_from_sf_id, &stones_1), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_get_stones(s_from_ss_id, &stones_2), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_1.stones[0].position.y, stones_2.stones[0].position.y);
    EXPECT_FLOAT_EQ(stones_1.stones[0].translational_velocity.y, stones_2.stones[0].translational_velocity.y);

    // 6. Clean up all C API instances
    EXPECT_EQ(dc_loader_remove_player_instance(post_reload_p_from_pf), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(post_reload_p_from_ps), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(p_from_pf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(p_from_ps_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(ps_id), DIGITALCURLING_OK);

    EXPECT_EQ(dc_loader_remove_simulator_instance(post_reload_s_from_sf), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(post_reload_s_from_ss), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(s_from_sf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(s_from_ss_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(sf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(ss_id), DIGITALCURLING_OK);
}

TEST_F(PluginLoaderDynamic, ConflictDifferentModule_RejectedAndLeavesOriginalInstancesUsable) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto player_res_before = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto sim_res_before = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(player_res_before, nullptr);
    ASSERT_NE(sim_res_before, nullptr);

    DigitalCurling_HandleId pf_id{}, ps_id{}, p_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_save(p_id, ps_id), DIGITALCURLING_OK);

    DigitalCurling_HandleId sf_id{}, ss_id{}, s_id{};
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, nullptr, &sf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, nullptr, &ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(sf_id, &s_id), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_to_set = {};
    stones_to_set.stones[0] = { {0.f, 9.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_EQ(dc_loader_simulator_set_stones(s_id, &stones_to_set), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_save(s_id, ss_id), DIGITALCURLING_OK);

    ScopedPluginCopy player_copy(player_plugin_path_, "conflict_loader_player");
    ScopedPluginCopy sim_copy(simulator_plugin_path_, "conflict_loader_sim");
    ASSERT_TRUE(player_copy.valid());
    ASSERT_TRUE(sim_copy.valid());

    DigitalCurling_PluginType type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256] = {};
    const std::string player_copy_str = player_copy.path().string();
    const std::string sim_copy_str = sim_copy.path().string();

    EXPECT_EQ(
        dc_loader_load_plugin(player_copy_str.c_str(), &type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
    );
    size_t err_size = 0;
    EXPECT_EQ(dc_loader_get_last_error_message(nullptr, 0, &err_size), DIGITALCURLING_OK);
    EXPECT_GT(err_size, 1u);

    EXPECT_EQ(
        dc_loader_load_plugin(sim_copy_str.c_str(), &type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
    );

    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), player_res_before);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName)), sim_res_before);

    // Pre-existing C API handles and registration remain usable after conflict rejection
    DigitalCurling_Shot input_shot = { 1.5f, 0.2f, 0.1f };
    DigitalCurling_Shot output_shot = {};
    ASSERT_EQ(dc_loader_player_play(p_id, &input_shot, &output_shot), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(output_shot.translational_velocity, input_shot.translational_velocity);

    DigitalCurling_HandleId p_from_storage_id{};
    ASSERT_EQ(dc_loader_create_player(ps_id, &p_from_storage_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_load(p_from_storage_id, ps_id), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_simulator_step(s_id, 1, 0.f), DIGITALCURLING_OK);
    DigitalCurling_HandleId s_from_storage_id{};
    ASSERT_EQ(dc_loader_create_simulator(ss_id, &s_from_storage_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_load(s_from_storage_id, ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_step(s_from_storage_id, 1, 0.f), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_1 = {}, stones_2 = {};
    ASSERT_EQ(dc_loader_simulator_get_stones(s_id, &stones_1), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_get_stones(s_from_storage_id, &stones_2), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(stones_1.stones[0].position.y, stones_2.stones[0].position.y);

    EXPECT_EQ(dc_loader_remove_player_instance(p_from_storage_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(p_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(ps_id), DIGITALCURLING_OK);

    EXPECT_EQ(dc_loader_remove_simulator_instance(s_from_storage_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(s_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(sf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(ss_id), DIGITALCURLING_OK);
}

TEST_F(PluginLoaderDynamic, BuiltInProtection_RejectsDynamicLoadAndPreservesLiveInstances) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    ScopedResourceRestore restore_guard(
        manager,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );

    auto builtin_player_res = detail::CreateTestPluginResource(player_plugin_path_, true);
    auto builtin_sim_res = detail::CreateTestPluginResource(simulator_plugin_path_, true);
    ASSERT_NE(builtin_player_res, nullptr);
    ASSERT_NE(builtin_sim_res, nullptr);

    detail::PluginManagerTestAccess::SetPlayerResource(manager, std::string(kPlayerPluginName), nullptr);
    detail::PluginManagerTestAccess::SetSimulatorResource(manager, std::string(kSimPluginName), nullptr);
    ASSERT_NO_THROW(manager.RegisterPlugin(builtin_player_res));
    ASSERT_NO_THROW(manager.RegisterPlugin(builtin_sim_res));

    DigitalCurling_HandleId pf_id{}, ps_id{}, p_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player_storage(kPlayerPluginName, nullptr, &ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_save(p_id, ps_id), DIGITALCURLING_OK);

    DigitalCurling_HandleId sf_id{}, ss_id{}, s_id{};
    ASSERT_EQ(dc_loader_create_simulator_factory(kSimPluginName, nullptr, &sf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator_storage(kSimPluginName, nullptr, &ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(sf_id, &s_id), DIGITALCURLING_OK);

    DigitalCurling_StoneCoordinate stones_to_set = {};
    stones_to_set.stones[0] = { {0.f, 7.f}, 0.f, {0.f, -1.f}, 0.f };
    ASSERT_EQ(dc_loader_simulator_set_stones(s_id, &stones_to_set), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_save(s_id, ss_id), DIGITALCURLING_OK);

    DigitalCurling_PluginType type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256] = {};
    EXPECT_EQ(
        dc_loader_load_plugin(player_plugin_path_.c_str(), &type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
    );
    EXPECT_EQ(
        dc_loader_load_plugin(simulator_plugin_path_.c_str(), &type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
    );

    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), builtin_player_res);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName)), builtin_sim_res);

    DigitalCurling_Shot input_shot = { 1.4f, 0.3f, 0.2f };
    DigitalCurling_Shot output_shot = {};
    ASSERT_EQ(dc_loader_player_play(p_id, &input_shot, &output_shot), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(output_shot.translational_velocity, input_shot.translational_velocity);

    DigitalCurling_HandleId p_from_ps_id{};
    ASSERT_EQ(dc_loader_create_player(ps_id, &p_from_ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_load(p_from_ps_id, ps_id), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_simulator_step(s_id, 1, 0.f), DIGITALCURLING_OK);
    DigitalCurling_HandleId s_from_ss_id{};
    ASSERT_EQ(dc_loader_create_simulator(ss_id, &s_from_ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_load(s_from_ss_id, ss_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_step(s_from_ss_id, 1, 0.f), DIGITALCURLING_OK);

    EXPECT_EQ(dc_loader_remove_player_instance(p_from_ps_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(p_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(ps_id), DIGITALCURLING_OK);

    EXPECT_EQ(dc_loader_remove_simulator_instance(s_from_ss_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(s_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(sf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(ss_id), DIGITALCURLING_OK);
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

std::string GetLoaderLastErrorMessage() {
    size_t required_size = 0;
    if (dc_loader_get_last_error_message(nullptr, 0, &required_size) != DIGITALCURLING_OK || required_size == 0) {
        return {};
    }
    std::vector<char> buffer(required_size);
    if (dc_loader_get_last_error_message(buffer.data(), buffer.size(), &required_size) != DIGITALCURLING_OK) {
        return {};
    }
    return std::string(buffer.data());
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

TEST_F(PluginLoaderDynamic, BareNameInControlledWorkingDirectory_LoadsRequestedModuleNotSearchPathDecoyAndHandlesNormalizedDuplicates) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    const std::filesystem::path test_bin_dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)).lexically_normal();
    const std::filesystem::path player_bin_dir = std::filesystem::absolute(std::filesystem::path(player_plugin_path_).parent_path()).lexically_normal();
    const std::string bare_name = "digitalcurling_core03_loader_bare_target" + LibraryExtension;

    ScopedTempDir work_dir(test_bin_dir / "core03_loader_controlled_cwd");
    ASSERT_TRUE(work_dir.valid());

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
    ScopedWindowsModule preloaded_decoy(decoy_in_test_dir.path());
    ASSERT_TRUE(preloaded_decoy.valid());
#endif

    ScopedCurrentPath cwd_guard(work_dir.path());
    ASSERT_TRUE(cwd_guard.valid());

    ScopedResourceRestore restore_guard(
        manager,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );
    detail::PluginManagerTestAccess::SetPlayerResource(manager, std::string(kPlayerPluginName), nullptr);

    // 1. Load by bare filename through C API from the controlled working directory
    DigitalCurling_PluginType loaded_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256] = {};
    ASSERT_EQ(
        dc_loader_load_plugin(bare_name.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_OK
    );
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
    EXPECT_STREQ(name_buffer, kPlayerPluginName);

    auto loaded_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
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

    DigitalCurling_HandleId pf_id{}, p_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_id), DIGITALCURLING_OK);

    // 2. Ordinary duplicate loading after normalization through C API
    const std::string dot_rel_str = PathToUtf8String(std::filesystem::path(".") / bare_name);
    const std::string parent_rel_str = PathToUtf8String(std::filesystem::path("..") / work_dir.path().filename() / "." / bare_name);
    const std::string exact_abs_str = PathToUtf8String(requested_in_cwd.path());
    const std::string unnormalized_abs_str = PathToUtf8String(work_dir.path() / "." / ".." / work_dir.path().filename() / bare_name);

    for (const auto& path_str : { bare_name, dot_rel_str, parent_rel_str, exact_abs_str, unnormalized_abs_str }) {
        loaded_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
        std::memset(name_buffer, 0, sizeof(name_buffer));
        ASSERT_EQ(
            dc_loader_load_plugin(path_str.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
            DIGITALCURLING_OK
        );
        EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
        EXPECT_STREQ(name_buffer, kPlayerPluginName);
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), loaded_res);
    }

    DigitalCurling_Shot shot_in = { 1.7f, 0.4f, 0.2f };
    DigitalCurling_Shot shot_out = {};
    ASSERT_EQ(dc_loader_player_play(p_id, &shot_in, &shot_out), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);

    EXPECT_EQ(dc_loader_remove_player_instance(p_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);
    loaded_res.reset();
}

TEST_F(PluginLoaderDynamic, NonAsciiUtf8Paths_LoadsAndReportsUtf8ErrorDiagnostics) {
    if (!IsPlayerPluginLoaded() || !IsSimPluginLoaded()) {
        GTEST_SKIP() << "Both plugins must be loaded for this test.";
    }

    auto& manager = PluginManager::GetInstance();
    auto orig_player_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    auto orig_sim_res = detail::PluginManagerTestAccess::GetSimulatorResource(manager, std::string(kSimPluginName));
    ASSERT_NE(orig_player_res, nullptr);
    ASSERT_NE(orig_sim_res, nullptr);

    const std::string non_ascii_dir_utf8 = "core03_loader_utf8_\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E_\xC3\xA4\xC3\xB6\xC3\xBC";
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
        manager,
        std::string(kPlayerPluginName),
        orig_player_res,
        std::string(kSimPluginName),
        orig_sim_res
    );
    detail::PluginManagerTestAccess::SetPlayerResource(manager, std::string(kPlayerPluginName), nullptr);

    // 1. Load via bare UTF-8 C string in non-ASCII working directory
    DigitalCurling_PluginType loaded_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name_buffer[256] = {};
    ASSERT_EQ(
        dc_loader_load_plugin(non_ascii_file_utf8.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_OK
    );
    EXPECT_EQ(loaded_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
    EXPECT_STREQ(name_buffer, kPlayerPluginName);

    auto loaded_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName));
    ASSERT_NE(loaded_res, nullptr);

    // 2. Duplicate load via full UTF-8 path and unnormalized UTF-8 path
    const std::string abs_utf8_str = PathToUtf8String(non_ascii_plugin_path);
    const std::string dot_utf8_str = PathToUtf8String(non_ascii_dir.path() / "." / std::filesystem::u8path(non_ascii_file_utf8));
    ASSERT_EQ(
        dc_loader_load_plugin(abs_utf8_str.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_OK
    );
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), loaded_res);

    ASSERT_EQ(
        dc_loader_load_plugin(dot_utf8_str.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_OK
    );
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, std::string(kPlayerPluginName)), loaded_res);

    DigitalCurling_HandleId pf_id{}, p_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kPlayerPluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_id), DIGITALCURLING_OK);
    DigitalCurling_Shot shot_in = { 2.3f, 0.6f, 0.1f };
    DigitalCurling_Shot shot_out = {};
    ASSERT_EQ(dc_loader_player_play(p_id, &shot_in, &shot_out), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);
    EXPECT_EQ(dc_loader_remove_player_instance(p_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);

    // 3. Missing non-ASCII UTF-8 path reports UTF-8 error message
    const std::string missing_utf8_str = PathToUtf8String(non_ascii_dir.path() / std::filesystem::u8path(missing_non_ascii_file_utf8));
    EXPECT_EQ(
        dc_loader_load_plugin(missing_utf8_str.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
        DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
    );
    const std::string err_msg = GetLoaderLastErrorMessage();
#ifdef _WIN32
    EXPECT_TRUE(IsValidUtf8Diagnostic(err_msg));
#endif
    EXPECT_NE(err_msg.find(non_ascii_dir_utf8), std::string::npos);
    EXPECT_NE(err_msg.find(missing_non_ascii_file_utf8), std::string::npos);

    loaded_res.reset();
}

TEST_F(PluginLoaderDynamic, MissingAndInvalidPaths_ReturnFailedToLoadPlugin) {
    const std::filesystem::path base_dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)).lexically_normal();
    ScopedTempDir temp_dir(base_dir / "core03_loader_invalid_paths");
    ASSERT_TRUE(temp_dir.valid());

    ScopedTempDir dir_with_ext(temp_dir.path() / ("directory_not_file" + LibraryExtension));
    ASSERT_TRUE(dir_with_ext.valid());

    const std::filesystem::path wrong_ext_file = temp_dir.path() / "not_a_plugin.txt";
    {
        std::ofstream ofs(wrong_ext_file, std::ios::binary);
        ofs << "dummy text content";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(wrong_ext_file));

    const std::filesystem::path no_ext_file = temp_dir.path() / "no_extension_file";
    {
        std::ofstream ofs(no_ext_file, std::ios::binary);
        ofs << "dummy content";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(no_ext_file));

    const std::filesystem::path invalid_binary_file = temp_dir.path() / ("invalid_binary" + LibraryExtension);
    {
        std::ofstream ofs(invalid_binary_file, std::ios::binary);
        ofs << "not a valid native shared library binary";
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(invalid_binary_file));

    const std::vector<std::string> invalid_utf8_paths = {
        "",
        "missing_bare_plugin" + LibraryExtension,
        PathToUtf8String(temp_dir.path() / ("missing_abs_plugin" + LibraryExtension)),
        PathToUtf8String(temp_dir.path()),
        PathToUtf8String(dir_with_ext.path()),
        PathToUtf8String(wrong_ext_file),
        PathToUtf8String(no_ext_file),
        PathToUtf8String(invalid_binary_file),
        PathToUtf8String(temp_dir.path() / ("bad_chars_<>:?*|" + LibraryExtension))
    };

    for (const auto& candidate : invalid_utf8_paths) {
        DigitalCurling_PluginType loaded_type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
        char name_buffer[256] = {};
        EXPECT_EQ(
            dc_loader_load_plugin(candidate.c_str(), &loaded_type, name_buffer, sizeof(name_buffer)),
            DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
        ) << "Unexpected error code for path: " << candidate;
        EXPECT_FALSE(GetLoaderLastErrorMessage().empty());
    }
}

#ifdef _WIN32
TEST_F(PluginLoaderDynamic, MalformedUtf8AndNativeFailure_ReportValidUtf8) {
    DigitalCurling_PluginType type = DIGITALCURLING_PLUGIN_TYPE_UNKNOWN;
    char name[256] = {};
    const std::string malformed = std::string("bad_") + "\xC3\x28" + LibraryExtension;
    EXPECT_EQ(dc_loader_load_plugin(malformed.c_str(), &type, name, sizeof(name)), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    EXPECT_TRUE(IsValidUtf8Diagnostic(GetLoaderLastErrorMessage()));

    const auto dir = std::filesystem::absolute(std::filesystem::path(DIGITALCURLING_TEST_DIR)) /
        std::filesystem::u8path("core03_error_\xE6\x97\xA5\xE6\x9C\xAC");
    ScopedTempDir temp(dir);
    ASSERT_TRUE(temp.valid());
    const auto path = dir / ("invalid_binary" + LibraryExtension);
    { std::ofstream file(path, std::ios::binary); file << "not a library"; }
    const auto input = PathToUtf8String(path);
    EXPECT_EQ(dc_loader_load_plugin(input.c_str(), &type, name, sizeof(name)), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    const auto error = GetLoaderLastErrorMessage();
    EXPECT_TRUE(IsValidUtf8Diagnostic(error));
    EXPECT_NE(error.find(input), std::string::npos);
    EXPECT_EQ(error.find("No error code available"), std::string::npos);
}
#endif

constexpr const char kLongNameFixturePluginName[] =
    "core04_long_name_player_plugin_fixture_0123456789_abcdefghijklmnopqrstuvwxyz_0123456789_abcdefghijklmnopqrstuvwxyz_end";

class ScopedSinglePlayerResourceRestore {
public:
    ScopedSinglePlayerResourceRestore(
        PluginManager& manager,
        std::string player_name,
        std::shared_ptr<detail::PlayerPluginResource> player_resource = nullptr
    )
        : manager_(manager),
          player_name_(std::move(player_name)),
          player_resource_(std::move(player_resource)) {}

    ~ScopedSinglePlayerResourceRestore() {
        detail::PluginManagerTestAccess::SetPlayerResource(manager_, player_name_, player_resource_);
    }

    ScopedSinglePlayerResourceRestore(const ScopedSinglePlayerResourceRestore&) = delete;
    ScopedSinglePlayerResourceRestore& operator=(const ScopedSinglePlayerResourceRestore&) = delete;

private:
    PluginManager& manager_;
    std::string player_name_;
    std::shared_ptr<detail::PlayerPluginResource> player_resource_;
};

TEST_F(PluginLoaderDynamic, LongNameFixture_BufferCapacityAtomicityAndOutputSentinels) {
    auto& manager = PluginManager::GetInstance();
    const std::string long_name_str(kLongNameFixturePluginName);
    const size_t long_name_len = long_name_str.size();
    ASSERT_GT(long_name_len, 64u);
    ASSERT_LT(long_name_len, 256u);

    const std::string fixture_macro_utf8 = DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH;
    ASSERT_FALSE(fixture_macro_utf8.empty()) << "DIGITALCURLING_TEST_LONG_NAME_PLUGIN_PATH must not be empty.";
    const std::filesystem::path fixture_path = std::filesystem::u8path(fixture_macro_utf8);
    ASSERT_TRUE(std::filesystem::exists(fixture_path))
        << "Long-name plugin fixture does not exist: " << PathToUtf8String(fixture_path);
    ASSERT_TRUE(std::filesystem::is_regular_file(fixture_path))
        << "Long-name plugin fixture is not a regular file: " << PathToUtf8String(fixture_path);
    const std::string fixture_path_str = PathToUtf8String(fixture_path);

    ScopedSinglePlayerResourceRestore cleanup_guard(
        manager,
        long_name_str,
        detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str)
    );
    detail::PluginManagerTestAccess::SetPlayerResource(manager, long_name_str, nullptr);

    constexpr DigitalCurling_PluginType kSentinelType = static_cast<DigitalCurling_PluginType>(0x5F);
    constexpr char kSentinelChar = '\x5A';
    const size_t backing_size = long_name_len + 16;

    auto verify_not_registered = [&]() {
        bool is_loaded = true;
        ASSERT_EQ(
            dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_PLAYER, kLongNameFixturePluginName, &is_loaded),
            DIGITALCURLING_OK
        );
        EXPECT_FALSE(is_loaded);
        EXPECT_FALSE(manager.IsPluginLoaded(PluginType::player, long_name_str));
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), nullptr);

        DigitalCurling_HandleId factory_id{};
        EXPECT_EQ(
            dc_loader_create_player_factory(kLongNameFixturePluginName, nullptr, &factory_id),
            DIGITALCURLING_ERR_PLUGIN_NOT_FOUND
        );
    };

    auto verify_buffer_untouched = [&](const std::vector<char>& buf) {
        EXPECT_TRUE(std::all_of(buf.begin(), buf.end(), [kSentinelChar](char c) { return c == kSentinelChar; }));
    };

    verify_not_registered();

    // 1. Null inputs on fresh unloaded fixture preserve non-null sentinels and add no registration
    {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        EXPECT_EQ(
            dc_loader_load_plugin(nullptr, &out_type, out_name.data(), out_name.size()),
            DIGITALCURLING_ERR_INVALID_ARGUMENT
        );
        EXPECT_EQ(out_type, kSentinelType);
        verify_buffer_untouched(out_name);
        EXPECT_FALSE(GetLoaderLastErrorMessage().empty());
        verify_not_registered();

        EXPECT_EQ(
            dc_loader_load_plugin(fixture_path_str.c_str(), nullptr, out_name.data(), out_name.size()),
            DIGITALCURLING_ERR_BUFFER_NULLPTR
        );
        verify_buffer_untouched(out_name);
        EXPECT_FALSE(GetLoaderLastErrorMessage().empty());
        verify_not_registered();

        EXPECT_EQ(
            dc_loader_load_plugin(fixture_path_str.c_str(), &out_type, nullptr, out_name.size()),
            DIGITALCURLING_ERR_BUFFER_NULLPTR
        );
        EXPECT_EQ(out_type, kSentinelType);
        EXPECT_FALSE(GetLoaderLastErrorMessage().empty());
        verify_not_registered();
    }

    // 2. Fresh fixture with insufficient name buffer (0, small, and exact name length bytes)
    for (const size_t insufficient_size : { size_t(0), size_t(1), size_t(8), size_t(64), long_name_len }) {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        EXPECT_EQ(
            dc_loader_load_plugin(fixture_path_str.c_str(), &out_type, out_name.data(), insufficient_size),
            DIGITALCURLING_ERR_BUFFER_TOO_SMALL
        ) << "Failed for insufficient buffer size: " << insufficient_size;
        EXPECT_EQ(out_type, kSentinelType) << "Output type modified for size: " << insufficient_size;
        verify_buffer_untouched(out_name);
        const std::string err_msg = GetLoaderLastErrorMessage();
        EXPECT_FALSE(err_msg.empty());
        EXPECT_EQ(err_msg.find("Loaded successfully"), std::string::npos);
        verify_not_registered();
    }

    // 3. Fresh fixture with exact adequate buffer (long_name_len + 1) succeeds, NUL-terminates, and registers
    {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        ASSERT_EQ(
            dc_loader_load_plugin(fixture_path_str.c_str(), &out_type, out_name.data(), long_name_len + 1),
            DIGITALCURLING_OK
        );
        EXPECT_EQ(out_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
        EXPECT_EQ(out_name[long_name_len], '\0');
        EXPECT_EQ(std::string_view(out_name.data(), long_name_len), long_name_str);
        EXPECT_STREQ(out_name.data(), kLongNameFixturePluginName);
        for (size_t i = long_name_len + 1; i < out_name.size(); ++i) {
            EXPECT_EQ(out_name[i], kSentinelChar) << "Guard byte overwritten at index " << i;
        }
    }

    bool is_loaded = false;
    ASSERT_EQ(
        dc_loader_is_plugin_loaded(DIGITALCURLING_PLUGIN_TYPE_PLAYER, kLongNameFixturePluginName, &is_loaded),
        DIGITALCURLING_OK
    );
    EXPECT_TRUE(is_loaded);
    EXPECT_TRUE(manager.IsPluginLoaded(PluginType::player, long_name_str));
    auto registered_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str);
    ASSERT_NE(registered_res, nullptr);

    DigitalCurling_HandleId pf_id{}, ps_id{}, p_id{};
    ASSERT_EQ(dc_loader_create_player_factory(kLongNameFixturePluginName, nullptr, &pf_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player_storage(kLongNameFixturePluginName, nullptr, &ps_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_player(pf_id, &p_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_player_save(p_id, ps_id), DIGITALCURLING_OK);

    // 4. Same-module reload with insufficient buffer preserves output sentinels and keeps live registration intact
    for (const size_t insufficient_size : { size_t(0), size_t(16), long_name_len }) {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        EXPECT_EQ(
            dc_loader_load_plugin(fixture_path_str.c_str(), &out_type, out_name.data(), insufficient_size),
            DIGITALCURLING_ERR_BUFFER_TOO_SMALL
        );
        EXPECT_EQ(out_type, kSentinelType);
        verify_buffer_untouched(out_name);
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), registered_res);
    }

    // 5. Same-module reload with adequate buffer is idempotent and preserves live instances
    const std::string alt_fixture_path_str = PathToUtf8String(fixture_path.parent_path() / "." / fixture_path.filename());
    for (const auto& reload_path : { fixture_path_str, alt_fixture_path_str }) {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        ASSERT_EQ(
            dc_loader_load_plugin(reload_path.c_str(), &out_type, out_name.data(), out_name.size()),
            DIGITALCURLING_OK
        );
        EXPECT_EQ(out_type, DIGITALCURLING_PLUGIN_TYPE_PLAYER);
        EXPECT_EQ(out_name[long_name_len], '\0');
        EXPECT_STREQ(out_name.data(), kLongNameFixturePluginName);
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), registered_res);
    }

    // 6. Collision with a different module copy preserves output sentinels and live registrations
    ScopedPluginCopy fixture_copy(fixture_path, "core04_collision_copy");
    ASSERT_TRUE(fixture_copy.valid());
    const std::string fixture_copy_str = PathToUtf8String(fixture_copy.path());

    for (const size_t collision_buf_size : { size_t(0), size_t(16), long_name_len }) {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        EXPECT_EQ(
            dc_loader_load_plugin(fixture_copy_str.c_str(), &out_type, out_name.data(), collision_buf_size),
            DIGITALCURLING_ERR_BUFFER_TOO_SMALL
        );
        EXPECT_EQ(out_type, kSentinelType);
        verify_buffer_untouched(out_name);
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), registered_res);
    }

    {
        DigitalCurling_PluginType out_type = kSentinelType;
        std::vector<char> out_name(backing_size, kSentinelChar);
        EXPECT_EQ(
            dc_loader_load_plugin(fixture_copy_str.c_str(), &out_type, out_name.data(), out_name.size()),
            DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN
        );
        EXPECT_EQ(out_type, kSentinelType);
        verify_buffer_untouched(out_name);
        EXPECT_FALSE(GetLoaderLastErrorMessage().empty());
        EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), registered_res);
    }

    DigitalCurling_Shot shot_in = { 2.1f, 0.3f, 0.2f };
    DigitalCurling_Shot shot_out = {};
    ASSERT_EQ(dc_loader_player_play(p_id, &shot_in, &shot_out), DIGITALCURLING_OK);
    EXPECT_FLOAT_EQ(shot_out.translational_velocity, shot_in.translational_velocity);
    EXPECT_FLOAT_EQ(shot_out.angular_velocity, shot_in.angular_velocity);
    EXPECT_FLOAT_EQ(shot_out.release_angle, shot_in.release_angle);

    EXPECT_EQ(dc_loader_remove_player_instance(p_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(pf_id), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_player_instance(ps_id), DIGITALCURLING_OK);
    registered_res.reset();

    // 7. Ordinary C++ PluginManager::LoadPlugin behavior with the long-name fixture
    detail::PluginManagerTestAccess::SetPlayerResource(manager, long_name_str, nullptr);
    verify_not_registered();

    std::optional<PluginId> cpp_loaded;
    ASSERT_NO_THROW(cpp_loaded = manager.LoadPlugin(fixture_path, true));
    ASSERT_TRUE(cpp_loaded.has_value());
    EXPECT_EQ(cpp_loaded->type, PluginType::player);
    EXPECT_EQ(cpp_loaded->name, long_name_str);
    EXPECT_TRUE(manager.IsPluginLoaded(PluginType::player, long_name_str));

    auto cpp_res = detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str);
    ASSERT_NE(cpp_res, nullptr);

    std::optional<PluginId> cpp_reloaded;
    ASSERT_NO_THROW(cpp_reloaded = manager.LoadPlugin(fixture_path, false));
    ASSERT_TRUE(cpp_reloaded.has_value());
    EXPECT_EQ(cpp_reloaded->type, PluginType::player);
    EXPECT_EQ(cpp_reloaded->name, long_name_str);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), cpp_res);

    std::optional<PluginId> cpp_conflict_no_throw;
    ASSERT_NO_THROW(cpp_conflict_no_throw = manager.LoadPlugin(fixture_copy.path(), false));
    EXPECT_FALSE(cpp_conflict_no_throw.has_value());
    EXPECT_THROW(manager.LoadPlugin(fixture_copy.path(), true), plugin_error);
    EXPECT_EQ(detail::PluginManagerTestAccess::GetPlayerResource(manager, long_name_str), cpp_res);

    auto cpp_factory = manager.CreatePlayerFactory(long_name_str);
    ASSERT_NE(cpp_factory, nullptr);
    EXPECT_EQ(cpp_factory->GetPlayerId(), long_name_str);
    auto cpp_storage = manager.CreatePlayerStorage(long_name_str);
    ASSERT_NE(cpp_storage, nullptr);
    EXPECT_EQ(cpp_storage->GetPlayerId(), long_name_str);
    auto cpp_player = cpp_factory->CreatePlayer();
    ASSERT_NE(cpp_player, nullptr);
    EXPECT_EQ(cpp_player->GetPlayerId(), long_name_str);
    moves::Shot cpp_shot = cpp_player->Play(moves::Shot{ 1.8f, 0.5f, 0.1f });
    EXPECT_FLOAT_EQ(cpp_shot.translational_velocity, 1.8f);
    EXPECT_FLOAT_EQ(cpp_shot.angular_velocity, 0.5f);
    EXPECT_FLOAT_EQ(cpp_shot.release_angle, 0.1f);

    cpp_player.reset();
    cpp_storage.reset();
    cpp_factory.reset();
    cpp_res.reset();
}

} // namespace
