#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "baseline_expected.hpp"
#include "common.hpp"
#include "../src/fcv1/simulator_fcv1_factory.hpp"
#include "../src/fcv1/simulator_fcv1_storage.hpp"
#include "../src/fcv1/simulator_fcv1.hpp"

namespace dct = digitalcurling::test;

TEST(SimulatorFCV1, InitialState)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    EXPECT_TRUE(simulator->AreAllStonesStopped());
}

TEST(SimulatorFCV1, SaveLoad1)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());

    {
        dcs::ISimulator::AllStones init_stones;
        init_stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        init_stones[15] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, -4.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        simulator->SetStones(init_stones);
    }

    // 1
    auto storage = simulator->CreateStorage();  // create & save
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones1 = simulator->GetStones();

    // 2
    simulator->Load(*storage);
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones2 = simulator->GetStones();

    EXPECT_TRUE(dct::EqualsSimulatorStones(stones1, stones2));
}

TEST(SimulatorFCV1, SaveLoad2)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    auto storage = simulator->CreateStorage();  // create & save

    {
        dcs::ISimulator::AllStones init_stones;
        init_stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        init_stones[15] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, -4.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        simulator->SetStones(init_stones);
    }

    // 1
    simulator->Save(*storage);
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones1 = simulator->GetStones();

    // 2
    simulator->Load(*storage);
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones2 = simulator->GetStones();

    EXPECT_TRUE(dct::EqualsSimulatorStones(stones1, stones2));
}

TEST(SimulatorFCV1, SaveLoad3)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());

    {
        dcs::ISimulator::AllStones init_stones;
        init_stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        init_stones[15] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, -4.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        simulator->SetStones(init_stones);
    }

    // 1
    auto storage = simulator->CreateStorage();  // create & save
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones1 = simulator->GetStones();

    // 2
    auto simulator_copy = dct::AsStepSimulator(storage->CreateSimulator());
    for (int i = 0; i < 10; ++i) { simulator_copy->Step(); }
    auto const stones2 = simulator_copy->GetStones();

    EXPECT_TRUE(dct::EqualsSimulatorStones(stones1, stones2));
}

TEST(SimulatorFCV1, SaveLoad4)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());

    {
        dcs::ISimulator::AllStones init_stones;
        init_stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        init_stones[15] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, -4.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
        simulator->SetStones(init_stones);
    }

    // 1
    std::shared_ptr<dcs::ISimulatorStorage> storage = simulator->CreateStorage();
    nlohmann::json j_storage = *std::dynamic_pointer_cast<dcs::SimulatorFCV1Storage>(storage);
    for (int i = 0; i < 10; ++i) { simulator->Step(); }
    auto const stones1 = simulator->GetStones();

    // 2
    auto storage2 = j_storage.get<dcs::SimulatorFCV1Storage>();
    auto simulator_copy = dct::AsStepSimulator(storage2.CreateSimulator());
    for (int i = 0; i < 10; ++i) { simulator_copy->Step(); }
    auto const stones2 = simulator_copy->GetStones();

    EXPECT_TRUE(dct::EqualsSimulatorStones(stones1, stones2));
}


TEST(SimulatorFCV1, FactoryToJson)
{
    dcs::SimulatorFCV1Factory factory;
    nlohmann::json const json = factory;
    EXPECT_EQ(json.at("type").get<std::string>(), "fcv1");
    EXPECT_EQ(json.at("seconds_per_frame").get<float>(), 0.001f);
}

TEST(SimulatorFCV1, FactoryFromJsonUsesFixedTimestep)
{
    nlohmann::json const minimal = {{"type", "fcv1"}};
    auto factory = minimal.get<dcs::SimulatorFCV1Factory>();
    EXPECT_FLOAT_EQ(dct::AsStepSimulator(factory.CreateSimulator())->GetSecondsPerFrame(), 0.001f);

    for (nlohmann::json const& legacy : {
             nlohmann::json(0.001f), nlohmann::json(0.25f),
             nlohmann::json("ignored"), nlohmann::json(nullptr)}) {
        auto input = minimal;
        input["seconds_per_frame"] = legacy;
        input.get_to(factory);
        EXPECT_FLOAT_EQ(dct::AsStepSimulator(factory.CreateSimulator())->GetSecondsPerFrame(), 0.001f);
    }

    EXPECT_THROW(
        nlohmann::json::array().get<dcs::SimulatorFCV1Factory>(),
        nlohmann::json::type_error);
}

TEST(SimulatorFCV1, StorageFactoryTimestepIsIgnored)
{
    dcs::SimulatorFCV1Factory factory;
    auto reference = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones init_stones;
    init_stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
    init_stones[15] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, -4.f), 0.5f, dc::Vector2(-0.1f, -2.f), 1.f);
    reference->SetStones(init_stones);
    nlohmann::json const base = reference->CreateStorage()->ToJson();
    EXPECT_EQ(base.at("factory").at("seconds_per_frame").get<float>(), 0.001f);
    for (int i = 0; i < 10; ++i) { reference->Step(); }
    auto const expected = dct::ToBaselineBits(reference->GetStones());

    for (bool const missing : {false, true}) {
        SCOPED_TRACE(missing);
        auto json = base;
        if (missing) {
            json["factory"].erase("seconds_per_frame");
        } else {
            json["factory"]["seconds_per_frame"] = 0.25f;
        }
        auto storage = json.get<dcs::SimulatorFCV1Storage>();
        auto simulator = dct::AsStepSimulator(storage.CreateSimulator());
        EXPECT_FLOAT_EQ(simulator->GetSecondsPerFrame(), 0.001f);
        for (int i = 0; i < 10; ++i) { simulator->Step(); }
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(simulator->GetStones()), expected));
    }
}

// Simulate() must give exactly what the old `SimulatorSimulate(Full, 0, width)` loop gave.
TEST(SimulatorFCV1, SimulateMatchesManualStepLoop)
{
    dcs::SimulatorFCV1Factory factory;
    for (auto const& scenario : dct::BaselineScenarios()) {
        SCOPED_TRACE(scenario.name);
        auto simulated = dct::AsStepSimulator(factory.CreateSimulator());
        auto stepped = dct::AsStepSimulator(factory.CreateSimulator());
        auto helper = dct::AsStepSimulator(factory.CreateSimulator());
        simulated->SetStones(scenario.initial);
        stepped->SetStones(scenario.initial);
        helper->SetStones(scenario.initial);

        simulated->Simulate(scenario.sheet_width);
        dct::ManualStepUntilRest(*stepped, scenario.sheet_width);
        auto const frames = dcs::StepUntilRest(*helper, 0, scenario.sheet_width);

        EXPECT_TRUE(simulated->AreAllStonesStopped());
        EXPECT_GT(frames, 0);
        auto const expected = dct::ToBaselineBits(stepped->GetStones());
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(simulated->GetStones()), expected));
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(helper->GetStones()), expected));
    }
}

TEST(SimulatorFCV1, StepUntilRestHonorsFrameLimit)
{
    dcs::SimulatorFCV1Factory factory;
    auto const& scenario = dct::BaselineScenarios().front();
    auto limited = dct::AsStepSimulator(factory.CreateSimulator());
    auto manual = dct::AsStepSimulator(factory.CreateSimulator());
    limited->SetStones(scenario.initial);
    manual->SetStones(scenario.initial);

    EXPECT_EQ(dcs::StepUntilRest(*limited, 10, scenario.sheet_width), 10);
    for (int i = 0; i < 10; ++i) {
        manual->Step();
    }
    EXPECT_TRUE(dct::BitsEqual(
        dct::ToBaselineBits(limited->GetStones()), dct::ToBaselineBits(manual->GetStones())));
    EXPECT_FALSE(limited->AreAllStonesStopped());

    // Stones that are already at rest are left alone and cost no frame.
    dcs::SimulatorFCV1Factory idle_factory;
    auto idle = dct::AsStepSimulator(idle_factory.CreateSimulator());
    EXPECT_EQ(dcs::StepUntilRest(*idle, 0, scenario.sheet_width), 0);
}

TEST(SimulatorFCV1, SimulateMatchesRecordedBaseline)
{
#ifdef DIGITALCURLING_TEST_HAVE_FCV1_BASELINE
    dcs::SimulatorFCV1Factory factory;
    auto const& scenarios = dct::BaselineScenarios();
    ASSERT_EQ(scenarios.size(), dct::kFcv1Baseline.size());
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        SCOPED_TRACE(scenarios[i].name);
        auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
        simulator->SetStones(scenarios[i].initial);
        simulator->Simulate(scenarios[i].sheet_width);
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(simulator->GetStones()), dct::kFcv1Baseline[i]));
    }
#else
    // No fcv1 baseline is recorded for this toolchain: compare with a step loop of the same build.
    dcs::SimulatorFCV1Factory factory;
    for (auto const& scenario : dct::BaselineScenarios()) {
        SCOPED_TRACE(scenario.name);
        auto simulated = dct::AsStepSimulator(factory.CreateSimulator());
        auto stepped = dct::AsStepSimulator(factory.CreateSimulator());
        simulated->SetStones(scenario.initial);
        stepped->SetStones(scenario.initial);
        simulated->Simulate(scenario.sheet_width);
        dct::ManualStepUntilRest(*stepped, scenario.sheet_width);
        EXPECT_TRUE(dct::BitsEqual(
            dct::ToBaselineBits(simulated->GetStones()), dct::ToBaselineBits(stepped->GetStones())));
    }
#endif
}

TEST(SimulatorFCV1, GetStonesAndGetFactoryReturnCopies)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    auto const& scenario = dct::BaselineScenarios().front();
    simulator->SetStones(scenario.initial);

    auto const stones_before = simulator->GetStones();
    auto const stones_before_bits = dct::ToBaselineBits(stones_before);
    auto factory_copy = simulator->GetFactory();
    auto storage = simulator->CreateStorage();
    ASSERT_NE(factory_copy, nullptr);
    EXPECT_NE(factory_copy.get(), simulator->GetFactory().get());
    EXPECT_EQ(factory_copy->GetSimulatorId(), simulator->GetSimulatorId());

    // Changing the simulator does not change what was returned earlier.
    simulator->Simulate(scenario.sheet_width);
    EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(stones_before), stones_before_bits));
    EXPECT_FALSE(dct::BitsEqual(dct::ToBaselineBits(simulator->GetStones()), stones_before_bits));

    simulator->Load(*storage);
    EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(simulator->GetStones()), stones_before_bits));
    EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(stones_before), stones_before_bits));

    // The factory copy still creates simulators with the same configuration.
    auto created = dct::AsStepSimulator(factory_copy->CreateSimulator());
    EXPECT_EQ(created->GetSecondsPerFrame(), simulator->GetSecondsPerFrame());
}

// Storage JSON written before collisions were removed still carries a "collisions" key. It must load, and the
// loaded simulator must hold the saved stones.
TEST(SimulatorFCV1, LoadsLegacyStorageJsonWithCollisions)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones stones;
    stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.1f, 2.f), 0.5f, dc::Vector2(0.f, 1.f), 1.f);
    stones[3] = dcs::ISimulator::StoneState(dc::Vector2(-0.4f, 8.f), 0.f, dc::Vector2(), 0.f);
    simulator->SetStones(stones);
    auto const expected = dct::ToBaselineBits(simulator->GetStones());

    nlohmann::json const current = simulator->CreateStorage()->ToJson();
    EXPECT_FALSE(current.contains("collisions"));

    nlohmann::json const entry = {
        {"a", {{"id", 0}, {"stone", {{"position", {{"x", 0.f}, {"y", 1.f}}}, {"angle", 0.f},
            {"translational_velocity", {{"x", 0.f}, {"y", 0.f}}}, {"angular_velocity", 0.f}}}}},
        {"b", {{"id", 1}, {"stone", {{"position", {{"x", 0.f}, {"y", 2.f}}}, {"angle", 0.f},
            {"translational_velocity", {{"x", 0.f}, {"y", 0.f}}}, {"angular_velocity", 0.f}}}}},
        {"normal_impulse", 1.f}, {"tangent_impulse", 0.f}};
    for (nlohmann::json const& collisions : {nlohmann::json::array(), nlohmann::json::array({entry})}) {
        auto legacy = current;
        legacy["collisions"] = collisions;
        auto storage = legacy.get<dcs::SimulatorFCV1Storage>();
        auto restored = dct::AsStepSimulator(storage.CreateSimulator());
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(restored->GetStones()), expected));
    }
}

TEST(SimulatorFCV1, RejectsNonFiniteStones)
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    dcs::SimulatorFCV1Factory factory;
    auto simulator = factory.CreateSimulator();

    auto const make = [](int field, float value) {
        dcs::ISimulator::StoneState stone(dc::Vector2(0.f, 2.f), 0.f, dc::Vector2(0.f, -1.f), 0.f);
        switch (field) {
        case 0: stone.position.x = value; break;
        case 1: stone.position.y = value; break;
        case 2: stone.angle = value; break;
        case 3: stone.translational_velocity.x = value; break;
        case 4: stone.translational_velocity.y = value; break;
        default: stone.angular_velocity = value; break;
        }
        return stone;
    };

    for (int field = 0; field < 6; ++field) {
        for (float const value : {nan, inf, -inf}) {
            auto const stone = make(field, value);
            dcs::ISimulator::AllStones stones;
            stones[3] = stone;
            EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument) << field;
            EXPECT_THROW(simulator->SetStone(dc::StoneIndex(3), stone), std::invalid_argument) << field;

            dcs::SimulatorFCV1Storage storage;
            storage.stones[3] = stone;
            EXPECT_THROW(simulator->Load(storage), std::invalid_argument) << field;
        }
    }

    // The failed calls must not have modified the simulator.
    for (auto const& stone : simulator->GetStones()) {
        EXPECT_FALSE(stone.has_value());
    }
}

TEST(SimulatorFCV1, StationaryStoneWithHugeAngularVelocityIsBounded)
{
    dcs::SimulatorFCV1Factory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones stones;
    stones[0] = dcs::ISimulator::StoneState(dc::Vector2(0.f, 2.f), 0.f, dc::Vector2(), 1e30f);
    simulator->SetStones(stones);

    bool finished = false;
    try {
        simulator->Simulate(dc::coordinate::kSheetWidthMax);
        finished = true;
    } catch (std::runtime_error const&) {
        // aborted at the frame limit
    }
    // Either the simulation ends or it is aborted; it must not run unbounded.
    EXPECT_TRUE(finished || !simulator->AreAllStonesStopped());
}

TEST(SimulatorFCV1, CalculateShotAcceptsValidTargets)
{
    dcs::SimulatorFCV1Factory factory;
    dcs::SimulatorFCV1 simulator(factory);
    auto const shot = simulator.CalculateShot(dc::Vector2(0.f, 35.f), 0.5f, 1.f);
    EXPECT_TRUE(std::isfinite(shot.translational_velocity));
    EXPECT_GT(shot.translational_velocity, 0.5f);
    EXPECT_TRUE(std::isfinite(shot.release_angle));
}

TEST(SimulatorFCV1, CalculateShotRejectsInvalidArguments)
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();
    dcs::SimulatorFCV1Factory factory;
    dcs::SimulatorFCV1 simulator(factory);

    // below the logarithm domain of each speed branch
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 20.f), 0.f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 25.f), 0.5f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 5.f), 2.f, 1.f), std::invalid_argument);
    // too far
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 60.5f), 0.f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(1e30f, 1e30f), 0.f, 1.f), std::invalid_argument);
    // non-finite
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(nan, 35.f), 0.5f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, inf), 0.5f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), nan, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), inf, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), 0.5f, nan), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), 0.5f, -inf), std::invalid_argument);
    // existing checks
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), -0.1f, 1.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 35.f), 0.5f, 0.f), std::invalid_argument);
    EXPECT_THROW(simulator.CalculateShot(dc::Vector2(0.f, 0.05f), 0.5f, 1.f), std::invalid_argument);
}
