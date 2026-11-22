#include <array>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#if defined(DIGITALCURLING_TEST_V2_INTERNAL_LIFETIME)
    #include "digitalcurling/plugins/detail/native_loader.hpp"
    #include "digitalcurling/plugins/detail/plugin_resource.hpp"
#endif
#include "digitalcurling/plugins/loader.h"
#include "digitalcurling/plugins/plugin_error.hpp"
#include "digitalcurling/plugins/plugin_manager.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"
#include "digitalcurling/simulators/plugin_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"

TEST(PluginLoaderV2Validation, InvalidV2IsRejected) {
    EXPECT_THROW(
        digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(DIGITALCURLING_INVALID_V2_TEST_PLUGIN, true),
        digitalcurling::plugins::plugin_error);
}

TEST(PluginLoaderV2Validation, RejectsModuleWithoutV2Entrypoint) {
    try {
        digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(DIGITALCURLING_V2_NO_ENTRYPOINT_PLUGIN, true);
        FAIL() << "Expected plugin_error for module without v2 entrypoint.";
    } catch (const digitalcurling::plugins::plugin_error& err) {
        EXPECT_EQ(err.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
    }
}

TEST(PluginLoaderV2Validation, RejectsMalformedDescriptors) {
    constexpr std::array paths{DIGITALCURLING_V2_NULL_PLUGIN, DIGITALCURLING_V2_MISSING_PLUGIN,
        DIGITALCURLING_V2_VERSION_PLUGIN, DIGITALCURLING_V2_SIZE_PLUGIN, DIGITALCURLING_V2_KIND_PLUGIN};
    for (const auto path : paths) {
        EXPECT_THROW(digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(path, true),
            digitalcurling::plugins::plugin_error) << path;
    }
}

TEST(PluginLoaderV2Validation, RejectsMalformedPluginNames) {
    constexpr std::array paths{DIGITALCURLING_V2_UNTERMINATED_NAME_PLUGIN,
        DIGITALCURLING_V2_INVALID_UTF8_NAME_PLUGIN, DIGITALCURLING_V2_CONTROL_NAME_PLUGIN};
    for (const auto path : paths) {
        EXPECT_THROW(digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(path, true),
            digitalcurling::plugins::plugin_error) << path;
    }
}

TEST(PluginLoaderV2Validation, OptionalCalculateShotIsUnsupported) {
    auto& manager = digitalcurling::plugins::PluginManager::GetInstance();
    ASSERT_NO_THROW(manager.LoadPlugin(DIGITALCURLING_V2_NO_SHOT_A_PLUGIN, true));
    EXPECT_EQ(manager.GetSimulatorCapabilities("v2-no-shot-a") &
        DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT, 0u);
    DigitalCurling_HandleId factory{};
    DigitalCurling_HandleId simulator{};
    ASSERT_EQ(dc_loader_create_simulator_factory("v2-no-shot-a", nullptr, &factory), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory, &simulator), DIGITALCURLING_OK);
    DigitalCurling_Vector2 position{};
    DigitalCurling_Shot shot{};
    EXPECT_EQ(dc_loader_simulator_calculate_shot(simulator, &position, 0.0f, 0.0f, &shot), 14);
    size_t message_size = 0;
    ASSERT_EQ(dc_loader_get_last_error_message(nullptr, 0, &message_size), DIGITALCURLING_OK);
    EXPECT_GT(message_size, 1u);
    EXPECT_EQ(dc_loader_remove_simulator_instance(simulator), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(factory), DIGITALCURLING_OK);

    auto cpp_factory = manager.CreateSimulatorFactory(std::string("v2-no-shot-a"));
    auto cpp_simulator = cpp_factory->CreateSimulator();
    EXPECT_EQ(dynamic_cast<digitalcurling::simulators::InvertiblePluginSimulator*>(cpp_simulator.get()), nullptr);
    EXPECT_EQ(dynamic_cast<digitalcurling::simulators::InvertiblePluginStepSimulator*>(cpp_simulator.get()), nullptr);
    EXPECT_NE(dynamic_cast<digitalcurling::simulators::PluginStepSimulator*>(cpp_simulator.get()), nullptr);
}

TEST(PluginLoaderV2Validation, RejectsPartialStepCallbackSets) {
    for (const auto path : {DIGITALCURLING_V2_PARTIAL_STEP_A_PLUGIN, DIGITALCURLING_V2_PARTIAL_STEP_B_PLUGIN}) {
        try {
            digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(path, true);
            FAIL() << "Expected plugin_error for a partial step callback set: " << path;
        } catch (const digitalcurling::plugins::plugin_error& err) {
            EXPECT_EQ(err.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN) << path;
        }
    }
}

TEST(PluginLoaderV2Validation, RejectsSubTableSizeMismatch) {
    for (const auto path : {DIGITALCURLING_V2_SIMULATOR_SIZE_PLUGIN, DIGITALCURLING_V2_PLAYER_SIZE_PLUGIN,
                            DIGITALCURLING_V2_PLAYER_TABLE_SIZE_PLUGIN, DIGITALCURLING_V2_PLAYER_SIMULATOR_SIZE_PLUGIN}) {
        try {
            digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(path, true);
            FAIL() << "Expected plugin_error for a sub-table size mismatch: " << path;
        } catch (const digitalcurling::plugins::plugin_error& err) {
            EXPECT_EQ(err.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN) << path;
        }
    }
}

// The fixture was built with the v2 layout of earlier v4.1 builds. Its callbacks abort the process,
// so this test also proves that no callback runs while the descriptor is rejected.
TEST(PluginLoaderV2Validation, RejectsPreChangeV2Layout) {
    try {
        digitalcurling::plugins::PluginManager::GetInstance().LoadPlugin(DIGITALCURLING_V2_STALE_LAYOUT_PLUGIN, true);
        FAIL() << "Expected plugin_error for a plugin built with the previous v2 layout.";
    } catch (const digitalcurling::plugins::plugin_error& err) {
        EXPECT_EQ(err.code(), DIGITALCURLING_ERR_FAILED_TO_LOAD_PLUGIN);
        EXPECT_NE(std::string(err.what()).find("Invalid v2 plugin descriptor"), std::string::npos) << err.what();
    }
    EXPECT_FALSE(digitalcurling::plugins::PluginManager::GetInstance().IsPluginLoaded(
        digitalcurling::plugins::PluginType::simulator, "v2-stale-layout"));
}

TEST(PluginLoaderV2Validation, SimulateOnlyPluginHasNoStepCapability) {
    auto& manager = digitalcurling::plugins::PluginManager::GetInstance();
    ASSERT_NO_THROW(manager.LoadPlugin(DIGITALCURLING_V2_SIMULATE_ONLY_PLUGIN, true));
    const auto capabilities = manager.GetSimulatorCapabilities("v2-simulate-only");
    EXPECT_EQ(capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_STEP, 0u);
    EXPECT_EQ(capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT, 0u);

    DigitalCurling_HandleId factory{};
    DigitalCurling_HandleId simulator{};
    ASSERT_EQ(dc_loader_create_simulator_factory("v2-simulate-only", nullptr, &factory), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory, &simulator), DIGITALCURLING_OK);

    DigitalCurling_PluginCapabilities reported = ~0u;
    ASSERT_EQ(dc_loader_simulator_get_capabilities(simulator, &reported), DIGITALCURLING_OK);
    EXPECT_EQ(reported & DIGITALCURLING_PLUGIN_CAPABILITY_STEP, 0u);

    // The step operations are unsupported (14) and say so in the last error message.
    auto last_error = [] {
        size_t required = 0;
        dc_loader_get_last_error_message(nullptr, 0, &required);
        std::string message(required, '\0');
        dc_loader_get_last_error_message(message.data(), message.size(), &required);
        return std::string(message.c_str());
    };
    EXPECT_EQ(dc_loader_simulator_step(simulator, 1, 4.75f), 14);
    EXPECT_NE(last_error().find("does not support stepping"), std::string::npos) << last_error();
    bool stopped = false;
    EXPECT_EQ(dc_loader_simulator_are_all_stones_stopped(simulator, &stopped), 14);
    EXPECT_NE(last_error().find("does not support stepping"), std::string::npos) << last_error();
    float seconds = 0.f;
    EXPECT_EQ(dc_loader_simulator_get_seconds_per_frame(simulator, &seconds), 14);
    EXPECT_NE(last_error().find("does not support stepping"), std::string::npos) << last_error();

    // Simulate goes through the plugin's simulate callback.
    DigitalCurling_StoneCoordinate stones{};
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator, &stones), DIGITALCURLING_OK);
    EXPECT_EQ(stones.stones[0].position.y, 1.f);
    ASSERT_EQ(dc_loader_simulator_simulate(simulator, 4.75f), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_get_stones(simulator, &stones), DIGITALCURLING_OK);
    EXPECT_EQ(stones.stones[0].position.y, 2.f);

    EXPECT_EQ(dc_loader_remove_simulator_instance(simulator), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(factory), DIGITALCURLING_OK);
}

namespace {

struct WrapperCase {
    const char* path;
    const char* name;
    bool step;
    bool shot;
};

void CheckWrapperSelection(digitalcurling::simulators::ISimulator& simulator, const WrapperCase& test_case) {
    using namespace digitalcurling::simulators;
    SCOPED_TRACE(test_case.name);

    auto* step = dynamic_cast<IStepSimulator*>(&simulator);
    EXPECT_EQ(step != nullptr, test_case.step);
    EXPECT_EQ(dynamic_cast<PluginStepSimulator*>(&simulator) != nullptr, test_case.step);
    EXPECT_EQ(dynamic_cast<PluginSimulator*>(&simulator) != nullptr, !test_case.step);
    EXPECT_EQ(dynamic_cast<InvertiblePluginStepSimulator*>(&simulator) != nullptr, test_case.step && test_case.shot);
    EXPECT_EQ(dynamic_cast<InvertiblePluginSimulator*>(&simulator) != nullptr, !test_case.step && test_case.shot);

    // Simulate runs the plugin's simulate; the fixture reports it as y = 2 of stone 0.
    EXPECT_EQ(simulator.GetStones()[0]->position.y, 1.f);
    ASSERT_NO_THROW(simulator.Simulate(4.75f));
    EXPECT_EQ(simulator.GetStones()[0]->position.y, 2.f);

    if (step) {
        // Step runs the plugin's step (y = 3 of stone 0).
        ASSERT_NO_THROW(step->Step());
        EXPECT_EQ(simulator.GetStones()[0]->position.y, 3.f);
        EXPECT_TRUE(step->AreAllStonesStopped());
        EXPECT_FLOAT_EQ(step->GetSecondsPerFrame(), 0.1f);
    }

    if (auto* invertible = dynamic_cast<InvertiblePluginSimulator*>(&simulator)) {
        EXPECT_FLOAT_EQ(invertible->CalculateShot(digitalcurling::Vector2(), 0.f, 1.f).translational_velocity, 1.f);
    } else if (auto* invertible_step = dynamic_cast<InvertiblePluginStepSimulator*>(&simulator)) {
        EXPECT_FLOAT_EQ(invertible_step->CalculateShot(digitalcurling::Vector2(), 0.f, 1.f).translational_velocity, 1.f);
    }
}

} // namespace

TEST(PluginLoaderV2Validation, WrapperMatchesStepAndCalculateShotCapabilities) {
    auto& manager = digitalcurling::plugins::PluginManager::GetInstance();
    const WrapperCase cases[] = {
        {DIGITALCURLING_V2_SIMULATE_ONLY_PLUGIN, "v2-simulate-only", false, false},
        {DIGITALCURLING_V2_SIMULATE_ONLY_SHOT_PLUGIN, "v2-simulate-only-shot", false, true},
        {DIGITALCURLING_V2_NO_SHOT_A_PLUGIN, "v2-no-shot-a", true, false},
        {DIGITALCURLING_V2_STEP_SHOT_PLUGIN, "v2-step-shot", true, true},
    };
    for (const auto& test_case : cases) {
        ASSERT_NO_THROW(manager.LoadPlugin(test_case.path, true)) << test_case.name;
        const auto capabilities = manager.GetSimulatorCapabilities(test_case.name);
        EXPECT_EQ((capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_STEP) != 0, test_case.step) << test_case.name;
        EXPECT_EQ((capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT) != 0, test_case.shot) << test_case.name;

        // Created through the factory ...
        auto factory = manager.CreateSimulatorFactory(std::string(test_case.name));
        auto from_factory = factory->CreateSimulator();
        ASSERT_NE(from_factory, nullptr);
        CheckWrapperSelection(*from_factory, test_case);

        // ... and through the storage.
        auto storage = manager.CreateSimulatorStorage(std::string(test_case.name));
        auto from_storage = storage->CreateSimulator();
        ASSERT_NE(from_storage, nullptr);
        CheckWrapperSelection(*from_storage, test_case);
    }
}

TEST(PluginLoaderV2Validation, RejectsStorageFromDifferentBackend) {
    auto& manager = digitalcurling::plugins::PluginManager::GetInstance();
    ASSERT_NO_THROW(manager.LoadPlugin(DIGITALCURLING_V2_NO_SHOT_A_PLUGIN, true));
    ASSERT_NO_THROW(manager.LoadPlugin(DIGITALCURLING_V2_NO_SHOT_B_PLUGIN, true));
    DigitalCurling_HandleId storage{};
    DigitalCurling_HandleId factory{};
    DigitalCurling_HandleId simulator{};
    ASSERT_EQ(dc_loader_create_simulator_storage("v2-no-shot-a", nullptr, &storage), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator_factory("v2-no-shot-b", nullptr, &factory), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory, &simulator), DIGITALCURLING_OK);
    EXPECT_NE(dc_loader_simulator_load(simulator, storage), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(simulator), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(factory), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_remove_simulator_instance(storage), DIGITALCURLING_OK);
}

TEST(PluginLoaderV2Validation, EmptySnapshotAndErrorCases) {
    auto& manager = digitalcurling::plugins::PluginManager::GetInstance();
    ASSERT_NO_THROW(manager.LoadPlugin(DIGITALCURLING_V2_NO_SHOT_A_PLUGIN, true));
    DigitalCurling_HandleId factory{};
    DigitalCurling_HandleId snapshot{};
    size_t snapshot_size = 0;
    ASSERT_EQ(dc_loader_create_simulator_factory("v2-no-shot-a", nullptr, &factory), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_creator_get_state_snapshot(factory, &snapshot, &snapshot_size), DIGITALCURLING_OK);
    EXPECT_EQ(snapshot_size, 1u);

    char buffer[2] = {'x', 'x'};
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot, buffer, 0), DIGITALCURLING_ERR_BUFFER_TOO_SMALL);
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot, buffer, 1), DIGITALCURLING_OK);
    EXPECT_EQ(buffer[0], '\0');
    EXPECT_EQ(dc_loader_destroy_snapshot(snapshot), DIGITALCURLING_OK);
    EXPECT_EQ(dc_loader_snapshot_get_data(snapshot, buffer, sizeof(buffer)), DIGITALCURLING_ERR_INSTANCE_NOT_FOUND);
    EXPECT_EQ(dc_loader_remove_simulator_instance(factory), DIGITALCURLING_OK);
}

#if defined(DIGITALCURLING_TEST_V2_INTERNAL_LIFETIME)
TEST(PluginLoaderV2Validation, RawProxyKeepsModulePinnedAfterResourceDestruction) {
    auto native = digitalcurling::plugins::detail::LoadNativePlugin(DIGITALCURLING_V2_NO_SHOT_A_PLUGIN);
    std::weak_ptr<void> module = native.handle;
    auto resource = digitalcurling::plugins::detail::PluginResource::CreateV2(
        native.get_plugin_api_v2(), std::move(native.handle));
    const auto id = resource->create_factory.Execute(nullptr);
    auto raw_handle = resource->GetInstanceList().Get<digitalcurling::plugins::FactoryHandle>(id);
    ASSERT_NE(raw_handle, nullptr);
    resource.reset();
    EXPECT_FALSE(module.expired());
    raw_handle.reset();
    EXPECT_TRUE(module.expired());
}
#endif
