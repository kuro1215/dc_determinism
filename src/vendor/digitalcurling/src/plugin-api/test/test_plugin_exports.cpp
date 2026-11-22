// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <vector>
#include "digitalcurling/players/i_player_factory.hpp"
#include "digitalcurling/players/i_player_storage.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#define DIGITALCURLING_PLUGIN_NAME "export-test"
#include "digitalcurling/plugins/player_plugin_export.hpp"
#include "digitalcurling/plugins/simulator_plugin_export.hpp"
#undef DIGITALCURLING_EXPORT_PLUGIN_FUNCTIONS
#define DIGITALCURLING_EXPORT_PLUGIN_FUNCTIONS(ApiInstance)

namespace {
using namespace digitalcurling;
namespace dp = digitalcurling::plugins;

struct Destruction {
    const void* address = nullptr;
    int count = 0;
    void Record(const void* p) { address = p; ++count; }
};

namespace player {
Destruction factories, storages, targets;
struct Target;
struct Factory final : players::IPlayerFactory {
    ~Factory() override { factories.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    players::Gender GetGender() const override { return {}; }
    std::unique_ptr<players::IPlayer> CreatePlayer() const override;
    std::unique_ptr<players::IPlayerFactory> Clone() const override { return std::make_unique<Factory>(*this); }
};
void from_json(const nlohmann::json&, Factory&) {}
struct Storage final : players::IPlayerStorage {
    ~Storage() override { storages.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    players::Gender GetGender() const override { return {}; }
    std::unique_ptr<players::IPlayer> CreatePlayer() const override;
};
void from_json(const nlohmann::json&, Storage&) {}
struct Target final : players::IPlayer {
    ~Target() override { targets.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    players::Gender GetGender() const override { return {}; }
    moves::Shot Play(const moves::Shot& shot) override { return shot; }
    std::unique_ptr<players::IPlayerFactory> GetFactory() const override { return std::make_unique<Factory>(); }
    std::unique_ptr<players::IPlayerStorage> CreateStorage() const override { return std::make_unique<Storage>(); }
    void Save(players::IPlayerStorage&) const override {}
    void Load(const players::IPlayerStorage&) override {}
};
std::unique_ptr<players::IPlayer> Factory::CreatePlayer() const { return std::make_unique<Target>(); }
std::unique_ptr<players::IPlayer> Storage::CreatePlayer() const { return std::make_unique<Target>(); }
DIGITALCURLING_EXPORT_PLAYER_PLUGIN(Factory, Storage, Target)
}

namespace simulator {
Destruction factories, storages, targets;
struct Target;
struct Factory final : simulators::ISimulatorFactory {
    ~Factory() override { factories.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    std::unique_ptr<simulators::ISimulator> CreateSimulator() const override;
    std::unique_ptr<simulators::ISimulatorFactory> Clone() const override { return std::make_unique<Factory>(*this); }
};
void from_json(const nlohmann::json&, Factory&) {}
struct Storage final : simulators::ISimulatorStorage {
    ~Storage() override { storages.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    std::unique_ptr<simulators::ISimulator> CreateSimulator() const override;
};
void from_json(const nlohmann::json&, Storage&) {}
struct Target final : simulators::IStepSimulator {
    ~Target() override { targets.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    void SetStones(const AllStones&) override {}
    void SetStone(StoneIndex, const std::optional<StoneState>&) override {}
    void Step() override {}
    AllStones GetStones() const override { return AllStones{}; }
    bool AreAllStonesStopped() const override { return true; }
    float GetSecondsPerFrame() const override { return 1.f; }
    std::unique_ptr<simulators::ISimulatorFactory> GetFactory() const override { return std::make_unique<Factory>(); }
    std::unique_ptr<simulators::ISimulatorStorage> CreateStorage() const override { return std::make_unique<Storage>(); }
    void Save(simulators::ISimulatorStorage&) const override {}
    void Load(const simulators::ISimulatorStorage&) override {}
};
std::unique_ptr<simulators::ISimulator> Factory::CreateSimulator() const { return std::make_unique<Target>(); }
std::unique_ptr<simulators::ISimulator> Storage::CreateSimulator() const { return std::make_unique<Target>(); }
DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN(Factory, Storage, Target)
}

// A simulator that only implements ISimulator::Simulate(), exported with the real export macro.
namespace simulate_only {
Destruction factories, storages, targets;
int simulate_calls = 0;
float last_width = 0.f;
struct Target;
struct Factory final : simulators::ISimulatorFactory {
    ~Factory() override { factories.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    std::unique_ptr<simulators::ISimulator> CreateSimulator() const override;
    std::unique_ptr<simulators::ISimulatorFactory> Clone() const override { return std::make_unique<Factory>(*this); }
};
void from_json(const nlohmann::json&, Factory&) {}
struct Storage final : simulators::ISimulatorStorage {
    ~Storage() override { storages.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    nlohmann::json ToJson() const override { return {}; }
    std::unique_ptr<simulators::ISimulator> CreateSimulator() const override;
};
void from_json(const nlohmann::json&, Storage&) {}
struct Target final : simulators::ISimulator {
    ~Target() override { targets.Record(this); }
    const char* GetId() const noexcept override { return "test"; }
    void SetStones(const AllStones&) override {}
    void SetStone(StoneIndex, const std::optional<StoneState>&) override {}
    void Simulate(float sheet_width) override { ++simulate_calls; last_width = sheet_width; }
    AllStones GetStones() const override { return AllStones{}; }
    std::unique_ptr<simulators::ISimulatorFactory> GetFactory() const override { return std::make_unique<Factory>(); }
    std::unique_ptr<simulators::ISimulatorStorage> CreateStorage() const override { return std::make_unique<Storage>(); }
    void Save(simulators::ISimulatorStorage&) const override {}
    void Load(const simulators::ISimulatorStorage&) override {}
};
std::unique_ptr<simulators::ISimulator> Factory::CreateSimulator() const { return std::make_unique<Target>(); }
std::unique_ptr<simulators::ISimulator> Storage::CreateSimulator() const { return std::make_unique<Target>(); }
DIGITALCURLING_EXPORT_SIMULATOR_PLUGIN(Factory, Storage, Target)
}

using PluginHandle = std::unique_ptr<void, DigitalCurling_PluginV2Destroy>;

template<class Handle>
void CheckDestroyed(PluginHandle& owner, Handle* handle, Destruction& record)
{
    ASSERT_NE(handle, nullptr);
    auto* complete = dynamic_cast<void*>(handle);
    EXPECT_NE(complete, static_cast<void*>(handle));
    const auto before = record.count;
    owner.reset();
    EXPECT_EQ(record.count, before + 1);
    EXPECT_EQ(record.address, complete);
}

template<class GetFactory>
void Exercise(const DigitalCurling_PluginApiV2& api, Destruction& factories, Destruction& storages,
              Destruction& targets, GetFactory get_factory)
{
    void* factory = nullptr;
    void* storage = nullptr;
    char* error = nullptr;
    const auto factory_result = api.create_factory(nullptr, &factory, &error);
    PluginHandle factory_owner(factory, api.destroy_factory);
    ASSERT_EQ(factory_result, DIGITALCURLING_PLUGIN_V2_OK);
    ASSERT_NE(factory, nullptr);
    const auto storage_result = api.create_storage(nullptr, &storage, &error);
    PluginHandle storage_owner(storage, api.destroy_storage);
    ASSERT_EQ(storage_result, DIGITALCURLING_PLUGIN_V2_OK);
    ASSERT_NE(storage, nullptr);
    for (void* creator : {factory, storage}) {
        void* target = nullptr;
        const auto target_result = api.create_target(creator, &target, &error);
        PluginHandle target_owner(target, api.destroy_target);
        ASSERT_EQ(target_result, DIGITALCURLING_PLUGIN_V2_OK);
        ASSERT_NE(target, nullptr);
        void* copy = nullptr;
        const auto copy_result = get_factory(target, &copy, &error);
        PluginHandle copy_owner(copy, api.destroy_factory);
        ASSERT_EQ(copy_result, DIGITALCURLING_PLUGIN_V2_OK);
        ASSERT_NE(copy, nullptr);
        CheckDestroyed(copy_owner, dp::detail::V2Box(copy)->factory, factories);
        CheckDestroyed(target_owner, dp::detail::V2Box(target)->target, targets);
    }
    CheckDestroyed(factory_owner, dp::detail::V2Box(factory)->factory, factories);
    CheckDestroyed(storage_owner, dp::detail::V2Box(storage)->storage, storages);
    const auto factory_count = factories.count;
    const auto storage_count = storages.count;
    const auto target_count = targets.count;
    api.destroy_factory(nullptr);
    api.destroy_storage(nullptr);
    api.destroy_target(nullptr);
    EXPECT_EQ(factories.count, factory_count);
    EXPECT_EQ(storages.count, storage_count);
    EXPECT_EQ(targets.count, target_count);
}

TEST(PluginExports, DescriptorSizesMatchTheSubTables)
{
    EXPECT_EQ(player::g_plugin_api_v2.struct_size, sizeof(DigitalCurling_PluginApiV2));
    EXPECT_EQ(player::g_plugin_api_v2.player_api_size, sizeof(DigitalCurling_PluginV2PlayerApi));
    EXPECT_EQ(player::g_plugin_api_v2.simulator_api_size, 0u);
    EXPECT_EQ(player::g_plugin_api_v2.simulator, nullptr);

    for (const auto* api : {&simulator::g_plugin_api_v2, &simulate_only::g_plugin_api_v2}) {
        EXPECT_EQ(api->struct_size, sizeof(DigitalCurling_PluginApiV2));
        EXPECT_EQ(api->simulator_api_size, sizeof(DigitalCurling_PluginV2SimulatorApi));
        EXPECT_EQ(api->player_api_size, 0u);
        EXPECT_EQ(api->player, nullptr);
        ASSERT_NE(api->simulator, nullptr);
    }
}

TEST(PluginExports, StepSimulatorExportsAllStepCallbacks)
{
    const auto& api = *simulator::g_plugin_api_v2.simulator;
    EXPECT_NE(api.step, nullptr);
    EXPECT_NE(api.are_all_stones_stopped, nullptr);
    EXPECT_NE(api.get_seconds_per_frame, nullptr);
    EXPECT_NE(api.simulate, nullptr);
}

TEST(PluginExports, SimulateOnlySimulatorExportsNoStepCallbacks)
{
    const auto& api = *simulate_only::g_plugin_api_v2.simulator;
    EXPECT_EQ(api.step, nullptr);
    EXPECT_EQ(api.are_all_stones_stopped, nullptr);
    EXPECT_EQ(api.get_seconds_per_frame, nullptr);
    EXPECT_NE(api.simulate, nullptr);
    EXPECT_NE(api.set_stones, nullptr);
    EXPECT_NE(api.set_stone, nullptr);
    EXPECT_NE(api.get_stones, nullptr);
    EXPECT_EQ(api.calculate_shot, nullptr);
}

TEST(PluginExports, SimulateOnlySimulatorDispatchesSimulate)
{
    const auto& api = simulate_only::g_plugin_api_v2;
    void* factory = nullptr;
    char* error = nullptr;
    ASSERT_EQ(api.create_factory(nullptr, &factory, &error), DIGITALCURLING_PLUGIN_V2_OK);
    PluginHandle factory_owner(factory, api.destroy_factory);
    void* target = nullptr;
    ASSERT_EQ(api.create_target(factory, &target, &error), DIGITALCURLING_PLUGIN_V2_OK);
    PluginHandle target_owner(target, api.destroy_target);

    const auto before = simulate_only::simulate_calls;
    EXPECT_EQ(api.simulator->simulate(target, 4.25f, &error), DIGITALCURLING_PLUGIN_V2_OK);
    EXPECT_EQ(simulate_only::simulate_calls, before + 1);
    EXPECT_EQ(simulate_only::last_width, 4.25f);
}

struct Handles {
    PluginHandle factory{nullptr, nullptr};
    PluginHandle storage{nullptr, nullptr};
    PluginHandle target{nullptr, nullptr};
};

void MakeHandles(const DigitalCurling_PluginApiV2& api, Handles& handles)
{
    void* factory = nullptr;
    void* storage = nullptr;
    void* target = nullptr;
    char* error = nullptr;
    ASSERT_EQ(api.create_factory(nullptr, &factory, &error), DIGITALCURLING_PLUGIN_V2_OK);
    handles.factory = PluginHandle(factory, api.destroy_factory);
    ASSERT_EQ(api.create_storage(nullptr, &storage, &error), DIGITALCURLING_PLUGIN_V2_OK);
    handles.storage = PluginHandle(storage, api.destroy_storage);
    ASSERT_EQ(api.create_target(factory, &target, &error), DIGITALCURLING_PLUGIN_V2_OK);
    handles.target = PluginHandle(target, api.destroy_target);
}

// Expects `status` to be a failure with `code`, and releases the error text.
void ExpectError(const DigitalCurling_PluginApiV2& api, DigitalCurling_PluginV2Status status, int code, char*& error)
{
    EXPECT_EQ(status, code);
    if (error) api.free_string(error);
    error = nullptr;
}

TEST(PluginExports, MismatchedBoxesAreRejected)
{
    const auto& player_api = player::g_plugin_api_v2;
    const auto& simulator_api = simulator::g_plugin_api_v2;
    Handles player_handles;
    Handles simulator_handles;
    MakeHandles(player_api, player_handles);
    MakeHandles(simulator_api, simulator_handles);
    ASSERT_NE(player_handles.target, nullptr);
    ASSERT_NE(simulator_handles.target, nullptr);
    char* error = nullptr;
    char* text = nullptr;
    void* out_box = nullptr;
    const int cast_error = DIGITALCURLING_ERR_FAILED_TO_CAST;

    // A target box is not a creator, and a storage box is not a factory.
    ExpectError(player_api, player_api.object_creator_get_state(player_handles.target.get(), &text, &error), cast_error, error);
    ExpectError(player_api, player_api.create_target(player_handles.target.get(), &out_box, &error), cast_error, error);
    ExpectError(player_api, player_api.factory_set_state(player_handles.storage.get(), "{}", &error), cast_error, error);
    ExpectError(player_api, player_api.storage_set_state(player_handles.factory.get(), "{}", &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.factory_set_state(simulator_handles.storage.get(), "{}", &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.storage_set_state(simulator_handles.factory.get(), "{}", &error), cast_error, error);

    // A simulator box passed to player functions, and the other way around.
    void* sim_box = simulator_handles.target.get();
    void* player_box = player_handles.target.get();
    int32_t gender = 0;
    DigitalCurling_PluginV2Shot shot{};
    DigitalCurling_PluginV2Shot out_shot{};
    ExpectError(player_api, player_api.player->get_gender(sim_box, &gender, &error), cast_error, error);
    ExpectError(player_api, player_api.player->play(sim_box, &shot, &out_shot, &error), cast_error, error);
    ExpectError(player_api, player_api.player->get_factory(sim_box, &out_box, &error), cast_error, error);
    ExpectError(player_api, player_api.player->save(sim_box, player_handles.storage.get(), &error), cast_error, error);
    ExpectError(player_api, player_api.player->load(sim_box, player_handles.storage.get(), &error), cast_error, error);
    ExpectError(player_api, player_api.player->save(player_box, simulator_handles.storage.get(), &error), cast_error, error);
    ExpectError(player_api, player_api.player->load(player_box, player_handles.factory.get(), &error), cast_error, error);

    DigitalCurling_PluginV2Bool stopped = 0;
    float seconds = 0.f;
    ExpectError(simulator_api, simulator_api.simulator->step(player_box, 1, 4.75f, &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->simulate(player_box, 4.75f, &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->are_all_stones_stopped(player_box, &stopped, &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->get_seconds_per_frame(player_box, &seconds, &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->get_factory(player_box, &out_box, &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->save(player_box, simulator_handles.storage.get(), &error), cast_error, error);
    ExpectError(simulator_api, simulator_api.simulator->load(sim_box, player_handles.storage.get(), &error), cast_error, error);
    EXPECT_EQ(out_box, nullptr);
    EXPECT_EQ(text, nullptr);

    // A null handle is an invalid argument.
    ExpectError(player_api, player_api.player->get_gender(nullptr, &gender, &error), DIGITALCURLING_ERR_INVALID_ARGUMENT, error);
    ExpectError(simulator_api, simulator_api.simulator->step(nullptr, 1, 4.75f, &error), DIGITALCURLING_ERR_INVALID_ARGUMENT, error);

    // Matching boxes still work.
    EXPECT_EQ(player_api.player->get_gender(player_box, &gender, &error), DIGITALCURLING_PLUGIN_V2_OK);
    EXPECT_EQ(simulator_api.simulator->are_all_stones_stopped(sim_box, &stopped, &error), DIGITALCURLING_PLUGIN_V2_OK);
    EXPECT_EQ(stopped, 1u);
}

TEST(PluginExports, MemoryWithoutBoxMagicIsRejected)
{
    const auto& player_api = player::g_plugin_api_v2;
    alignas(dp::detail::V2OwnedBox) unsigned char junk[sizeof(dp::detail::V2OwnedBox)] = {};
    char* error = nullptr;
    int32_t gender = 0;
    ExpectError(player_api, player_api.player->get_gender(junk, &gender, &error), DIGITALCURLING_ERR_FAILED_TO_CAST, error);
    // Destroying a foreign pointer must neither crash nor free it.
    player_api.destroy_target(junk);
    player_api.destroy_factory(junk);
}

TEST(PluginExports, PlayerHandles)
{
    Exercise(player::g_plugin_api_v2, player::factories, player::storages, player::targets,
        player::g_plugin_api_v2.player->get_factory);
}
TEST(PluginExports, SimulatorHandles)
{
    Exercise(simulator::g_plugin_api_v2, simulator::factories, simulator::storages, simulator::targets,
        simulator::g_plugin_api_v2.simulator->get_factory);
}
TEST(PluginExports, SimulateOnlySimulatorHandles)
{
    Exercise(simulate_only::g_plugin_api_v2, simulate_only::factories, simulate_only::storages, simulate_only::targets,
        simulate_only::g_plugin_api_v2.simulator->get_factory);
}
} // namespace
