// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/plugins/loader.h"
#include "digitalcurling/simulators/plugin_simulator.hpp"
#include "digitalcurling/simulators/plugin_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"
#include "common.hpp"
#include "loader_test_base.hpp"
#include "../../simulator/test/baseline_expected.hpp"

std::string digitalcurling::test::GetSnapshotData(DigitalCurling_HandleId id, size_t snapshot_size) {
    if (snapshot_size == 0) {
        dc_loader_destroy_snapshot(id);
        return "";
    }

    std::vector<char> buffer(snapshot_size);
    DigitalCurling_ErrorCode err = dc_loader_snapshot_get_data(id, buffer.data(), buffer.size());
    dc_loader_destroy_snapshot(id);

    return err == DIGITALCURLING_OK ? std::string(buffer.data()) : "";
}

namespace {

bool SameBits(float a, float b) {
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

bool SameStones(digitalcurling::simulators::ISimulator::AllStones const& a,
                digitalcurling::simulators::ISimulator::AllStones const& b) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].has_value() != b[i].has_value()) return false;
        if (!a[i].has_value()) continue;
        if (!SameBits(a[i]->position.x, b[i]->position.x) || !SameBits(a[i]->position.y, b[i]->position.y) ||
            !SameBits(a[i]->angle, b[i]->angle) ||
            !SameBits(a[i]->translational_velocity.x, b[i]->translational_velocity.x) ||
            !SameBits(a[i]->translational_velocity.y, b[i]->translational_velocity.y) ||
            !SameBits(a[i]->angular_velocity, b[i]->angular_velocity)) return false;
    }
    return true;
}

} // namespace

void digitalcurling::test::CheckLoaderSimulate(const char* simulator_plugin_name) {
    DigitalCurling_HandleId factory_id{}, simulate_id{}, step_id{};
    ASSERT_EQ(dc_loader_create_simulator_factory(simulator_plugin_name, nullptr, &factory_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &simulate_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_create_simulator(factory_id, &step_id), DIGITALCURLING_OK);

    DigitalCurling_PluginCapabilities capabilities = 0;
    ASSERT_EQ(dc_loader_simulator_get_capabilities(simulate_id, &capabilities), DIGITALCURLING_OK);
    EXPECT_NE(capabilities & DIGITALCURLING_PLUGIN_CAPABILITY_STEP, 0u);
    EXPECT_EQ(capabilities & 1u, 0u); // bit 0 is reserved (formerly SET_STONE) and never set

    // A shot to the tee, and a stone that leaves the sheet sideways.
    DigitalCurling_Shot shot{};
    DigitalCurling_Vector2 tee = { coordinate::kTee.x, coordinate::kTee.y };
    ASSERT_EQ(dc_loader_simulator_calculate_shot(simulate_id, &tee, 0.2f, kHalfPi, &shot), DIGITALCURLING_OK);
    DigitalCurling_StoneCoordinate initial = {};
    initial.stones[0] = { {0.f, 0.f}, 0.f,
        {shot.translational_velocity * std::cos(shot.release_angle), shot.translational_velocity * std::sin(shot.release_angle)},
        kHalfPi };
    initial.stones[1] = { {0.f, 10.f}, 0.f, {3.5f, 1.f}, 1.f };
    ASSERT_EQ(dc_loader_simulator_set_stones(simulate_id, &initial), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_simulator_set_stones(step_id, &initial), DIGITALCURLING_OK);

    ASSERT_EQ(dc_loader_simulator_simulate(simulate_id, coordinate::kSheetWidthMax), DIGITALCURLING_OK);
    bool stopped = false;
    ASSERT_EQ(dc_loader_simulator_are_all_stones_stopped(simulate_id, &stopped), DIGITALCURLING_OK);
    EXPECT_TRUE(stopped);
    DigitalCurling_StoneCoordinate simulated = {};
    ASSERT_EQ(dc_loader_simulator_get_stones(simulate_id, &simulated), DIGITALCURLING_OK);

    // The reference: step frame by frame until every stone is at rest.
    bool step_stopped = false;
    for (int i = 0; i < 1000000 && !step_stopped; ++i) {
        ASSERT_EQ(dc_loader_simulator_step(step_id, 1, coordinate::kSheetWidthMax), DIGITALCURLING_OK);
        ASSERT_EQ(dc_loader_simulator_are_all_stones_stopped(step_id, &step_stopped), DIGITALCURLING_OK);
    }
    ASSERT_TRUE(step_stopped);
    DigitalCurling_StoneCoordinate stepped = {};
    ASSERT_EQ(dc_loader_simulator_get_stones(step_id, &stepped), DIGITALCURLING_OK);
    EXPECT_EQ(std::memcmp(&simulated, &stepped, sizeof(simulated)), 0);

    // The draw stops near the tee; the other stone was removed (absent stones are all zero).
    EXPECT_NEAR(simulated.stones[0].position.y, coordinate::kTee.y, 3.0f);
    EXPECT_EQ(simulated.stones[1].position.x, 0.f);
    EXPECT_EQ(simulated.stones[1].position.y, 0.f);

    // Simulating stones that are already at rest changes nothing, and an invalid width falls back to the maximum.
    for (float width : { coordinate::kSheetWidthMax, 0.f, std::numeric_limits<float>::quiet_NaN() }) {
        ASSERT_EQ(dc_loader_simulator_simulate(simulate_id, width), DIGITALCURLING_OK);
        DigitalCurling_StoneCoordinate again = {};
        ASSERT_EQ(dc_loader_simulator_get_stones(simulate_id, &again), DIGITALCURLING_OK);
        EXPECT_EQ(std::memcmp(&simulated, &again, sizeof(simulated)), 0);
    }

    ASSERT_EQ(dc_loader_remove_simulator_instance(simulate_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(step_id), DIGITALCURLING_OK);
    ASSERT_EQ(dc_loader_remove_simulator_instance(factory_id), DIGITALCURLING_OK);
}

void digitalcurling::test::CheckWrapperSimulate(simulators::ISimulatorFactory const& factory) {
    auto simulated = ToStepSimulator(factory.CreateSimulator());
    auto stepped = ToStepSimulator(factory.CreateSimulator());
    auto* invertible = dynamic_cast<simulators::InvertiblePluginStepSimulator*>(simulated.get());
    ASSERT_NE(invertible, nullptr);
    auto* stepping = dynamic_cast<simulators::PluginStepSimulator*>(stepped.get());
    ASSERT_NE(stepping, nullptr);
    // A step-capable plugin is never wrapped as a simulate-only simulator.
    EXPECT_EQ(dynamic_cast<simulators::PluginSimulator*>(static_cast<simulators::ISimulator*>(simulated.get())), nullptr);

    moves::Shot shot{};
    ASSERT_NO_THROW(shot = invertible->CalculateShot(coordinate::kTee, 0.2f, kHalfPi));
    simulators::ISimulator::AllStones initial;
    initial[0] = simulators::ISimulator::StoneState{ {0.f, 0.f}, 0.f, shot.ToVector2(), shot.angular_velocity };
    initial[1] = simulators::ISimulator::StoneState{ {0.f, 10.f}, 0.f, {3.5f, 1.f}, 1.f };
    ASSERT_NO_THROW(simulated->SetStones(initial));
    ASSERT_NO_THROW(stepped->SetStones(initial));

    ASSERT_NO_THROW(simulated->Simulate(coordinate::kSheetWidthMax));
    EXPECT_TRUE(simulated->AreAllStonesStopped());
    for (int i = 0; i < 1000000 && !stepped->AreAllStonesStopped(); ++i) {
        ASSERT_NO_THROW(stepping->Step(1, coordinate::kSheetWidthMax));
    }
    ASSERT_TRUE(stepped->AreAllStonesStopped());

    auto const simulated_stones = simulated->GetStones();
    EXPECT_TRUE(SameStones(simulated_stones, stepped->GetStones()));
    ASSERT_TRUE(simulated_stones[0].has_value());
    EXPECT_NEAR(simulated_stones[0]->position.y, coordinate::kTee.y, 3.0f);
    EXPECT_FALSE(simulated_stones[1].has_value());

    // Values returned by GetStones() and GetFactory() are copies: they stay valid after later calls.
    auto const stones_copy = simulated->GetStones();
    auto factory_copy = simulated->GetFactory();
    auto storage = simulated->CreateStorage();
    ASSERT_NO_THROW(simulated->SetStones(initial));
    ASSERT_NO_THROW(simulated->Simulate(coordinate::kSheetWidthMax));
    ASSERT_NO_THROW(simulated->Load(*storage));
    ASSERT_NE(factory_copy, nullptr);
    EXPECT_EQ(factory_copy->GetSimulatorId(), simulated->GetSimulatorId());
    EXPECT_TRUE(SameStones(stones_copy, simulated_stones));
    EXPECT_TRUE(SameStones(simulated->GetStones(), simulated_stones));
    EXPECT_NE(factory_copy->CreateSimulator(), nullptr);
}

void digitalcurling::test::CheckLoaderBaseline(const char* plugin_name) {
    const bool is_fcv1plus = std::string(plugin_name) == "fcv1plus";
    auto const& scenarios = BaselineScenarios();
    for (std::size_t index = 0; index < scenarios.size(); ++index) {
        auto const& scenario = scenarios[index];
        SCOPED_TRACE(std::string(plugin_name) + " / " + scenario.name);

        DigitalCurling_HandleId factory_id{}, simulator_id{};
        ASSERT_EQ(dc_loader_create_simulator_factory(plugin_name, nullptr, &factory_id), DIGITALCURLING_OK);
        ASSERT_EQ(dc_loader_create_simulator(factory_id, &simulator_id), DIGITALCURLING_OK);

        DigitalCurling_StoneCoordinate coordinate_in = {};
        for (std::size_t i = 0; i < scenario.initial.size(); ++i) {
            if (!scenario.initial[i].has_value()) continue;
            auto const& s = *scenario.initial[i];
            coordinate_in.stones[i] = { {s.position.x, s.position.y}, s.angle,
                                        {s.translational_velocity.x, s.translational_velocity.y}, s.angular_velocity };
        }
        ASSERT_EQ(dc_loader_simulator_set_stones(simulator_id, &coordinate_in), DIGITALCURLING_OK);
        ASSERT_EQ(dc_loader_simulator_simulate(simulator_id, scenario.sheet_width), DIGITALCURLING_OK);
        DigitalCurling_StoneCoordinate coordinate_out = {};
        ASSERT_EQ(dc_loader_simulator_get_stones(simulator_id, &coordinate_out), DIGITALCURLING_OK);

        // An absent stone is reported as all zeros.
        simulators::ISimulator::AllStones stones;
        for (std::size_t i = 0; i < stones.size(); ++i) {
            auto const& c = coordinate_out.stones[i];
            if (c.position.x == 0.f && c.position.y == 0.f && c.angle == 0.f &&
                c.translational_velocity.x == 0.f && c.translational_velocity.y == 0.f && c.angular_velocity == 0.f) continue;
            stones[i] = simulators::ISimulator::StoneState{
                {c.position.x, c.position.y}, c.angle,
                {c.translational_velocity.x, c.translational_velocity.y}, c.angular_velocity };
        }
        if (is_fcv1plus) {
            EXPECT_TRUE(BitsEqual(ToBaselineBits(stones), kFcv1PlusBaseline[index]));
        } else {
#ifdef DIGITALCURLING_TEST_HAVE_FCV1_BASELINE
            EXPECT_TRUE(BitsEqual(ToBaselineBits(stones), kFcv1Baseline[index]));
#endif
        }

        ASSERT_EQ(dc_loader_remove_simulator_instance(simulator_id), DIGITALCURLING_OK);
        ASSERT_EQ(dc_loader_remove_simulator_instance(factory_id), DIGITALCURLING_OK);
    }
}
