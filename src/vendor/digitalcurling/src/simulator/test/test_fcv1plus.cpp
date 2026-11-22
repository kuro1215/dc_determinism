#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "baseline_expected.hpp"
#include "common.hpp"
#ifdef DIGITALCURLING_TEST_HAVE_FCV1
#include "../src/fcv1/simulator_fcv1_factory.hpp"
#include "../src/fcv1/simulator_fcv1.hpp"
#endif
#include "../src/fcv1plus/collision_solver.hpp"
#include "../src/fcv1plus/simulator_fcv1plus.hpp"
#include "../src/fcv1plus/simulator_fcv1plus_factory.hpp"
#include "../src/fcv1plus/simulator_fcv1plus_storage.hpp"

namespace dct = digitalcurling::test;
namespace fcp = digitalcurling::simulators::fcv1plus_detail;
namespace solver = digitalcurling::simulators::fcv1plus_detail::collision_solver;

namespace {

constexpr double kQ824Ulp = 1.0 / 16777216.0;

// Derived before the first test run. The exact-contact head-on Mode B path is
// the stricter of the two modes: N=38 Fixed-on-Fixed add/sub/mul/div
// operations that can change this result. Later ordered passes only revisit
// already-separating exact contacts. The collision path uses the integer
// square root, not an FPM approximation.
constexpr double kCollisionConservationTolerance =
    38.0 * kQ824Ulp; // 2.26497650146484375e-6

// Mode B first quantizes the tangential impulse to Q8.24, then multiplies
// and divides it by the 0.145 m lever arm to update angular velocity. These
// bounds cover that independent fixed-point rounding without weakening the
// analytic collision-model assertions below.
constexpr double kModeBImpulseTolerance = 128.0 * kQ824Ulp;
constexpr double kModeBAngularVelocityTolerance =
    (2.0 / dc::Stone::kRadius + 1.0) * 96.0 * kQ824Ulp;

// FPM (b46537f) does not publish ULP guarantees for all approximations. Its
// source tests provide these conservative relative ceilings instead:
// sqrt(Q8.24)=5e-7, log=1e-2, pow=1.1e-1, atan2=2.5e-2 per call; math.hpp
// documents sin's worst case as 7e-4 (cos delegates to sin). The longest
// closed-form CalculateShot regression branch has N=16 operations.
constexpr double kCalculateShotRegressionRelativeTolerance =
    16.0 * kQ824Ulp + 5.0e-7 + 1.0e-2;
constexpr double kCalculateShotFullRelativeTolerance =
    16.0 * kQ824Ulp + 5.0e-7 + 1.0e-2 + 1.1e-1 +
    7.0e-4 + 2.0 * 2.5e-2;
constexpr double kCalculateShotScalarTolerance =
    5.0 * kCalculateShotRegressionRelativeTolerance;
constexpr double kCalculateShotAngleTolerance =
    6.283185307179586 * kCalculateShotFullRelativeTolerance;
constexpr double kCalculateShotDistanceTolerance =
    50.0 * kCalculateShotFullRelativeTolerance; // 8.535072683715821 m

fcp::StoneStateFixed MakeFixedStone(
    float x,
    float y,
    float velocity_x = 0.0f,
    float velocity_y = 0.0f,
    float angular_velocity = 0.0f)
{
    fcp::StoneStateFixed result;
    result.enabled = true;
    result.position = {fcp::Fixed(x), fcp::Fixed(y)};
    result.velocity = {fcp::Fixed(velocity_x), fcp::Fixed(velocity_y)};
    result.angular_velocity = fcp::Fixed(angular_velocity);
    return result;
}

dcs::SimulatorFCV1PlusRawStone MakeRawStone(
    float x,
    float y,
    float velocity_x = 0.0f,
    float velocity_y = 0.0f,
    float angular_velocity = 0.0f)
{
    dcs::SimulatorFCV1PlusRawStone result;
    result.enabled = true;
    result.position_x = fcp::Fixed(x).raw_value();
    result.position_y = fcp::Fixed(y).raw_value();
    result.velocity_x = fcp::Fixed(velocity_x).raw_value();
    result.velocity_y = fcp::Fixed(velocity_y).raw_value();
    result.angular_velocity = fcp::Fixed(angular_velocity).raw_value();
    return result;
}

double TranslationalEnergy(fcp::AllStonesFixed const& stones)
{
    double result = 0.0;
    for (auto const& stone : stones) {
        if (!stone.enabled) {
            continue;
        }
        auto const x = static_cast<double>(stone.velocity.x);
        auto const y = static_cast<double>(stone.velocity.y);
        result += 0.5 * (x * x + y * y);
    }
    return result;
}

double TotalUnitMassEnergy(fcp::AllStonesFixed const& stones)
{
    auto result = TranslationalEnergy(stones);
    for (auto const& stone : stones) {
        if (!stone.enabled) {
            continue;
        }
        auto const omega = static_cast<double>(stone.angular_velocity);
        result += 0.25 * dc::Stone::kRadius * dc::Stone::kRadius *
                  omega * omega;
    }
    return result;
}

double TotalUnitMassAngularMomentum(fcp::AllStonesFixed const& stones)
{
    double result = 0.0;
    for (auto const& stone : stones) {
        if (!stone.enabled) {
            continue;
        }
        auto const x = static_cast<double>(stone.position.x);
        auto const y = static_cast<double>(stone.position.y);
        auto const velocity_x = static_cast<double>(stone.velocity.x);
        auto const velocity_y = static_cast<double>(stone.velocity.y);
        auto const omega = static_cast<double>(stone.angular_velocity);
        result += x * velocity_y - y * velocity_x;
        result += 0.5 * dc::Stone::kRadius * dc::Stone::kRadius * omega;
    }
    return result;
}

std::uint64_t HashRawState(
    dcs::SimulatorFCV1PlusStorage::RawStones const& stones)
{
    // Cross-platform golden serialization:
    // u32 schema version (little endian), then 16 records in index order,
    // each u8 enabled followed by six signed int32 two's-complement fields in
    // little endian. Hash is 64-bit FNV-1a over exactly those bytes.
    std::uint64_t hash = 14695981039346656037ull;
    auto append_byte = [&](std::uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    auto append_u32 = [&](std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            append_byte(static_cast<std::uint8_t>(value >> shift));
        }
    };
    append_u32(dcs::SimulatorFCV1PlusStorage::kSchemaVersion);
    for (auto const& stone : stones) {
        append_byte(stone.enabled ? 1u : 0u);
        for (auto value : {
                 stone.position_x,
                 stone.position_y,
                 stone.angle,
                 stone.velocity_x,
                 stone.velocity_y,
                 stone.angular_velocity})
        {
            append_u32(static_cast<std::uint32_t>(value));
        }
    }
    return hash;
}

dcs::ISimulator::AllStones SaveLoadInitialStones()
{
    dcs::ISimulator::AllStones result;
    result[0].emplace(
        dc::Vector2(0.1f, 2.0f),
        0.5f,
        dc::Vector2(-0.1f, -2.0f),
        1.0f);
    result[15].emplace(
        dc::Vector2(0.1f, 4.0f),
        -0.5f,
        dc::Vector2(-0.1f, -2.0f),
        -1.0f);
    return result;
}

} // namespace

TEST(SimulatorFCV1Plus, InitialState)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    EXPECT_TRUE(simulator->AreAllStonesStopped());
    EXPECT_EQ(simulator->GetSecondsPerFrame(), 0.001f);
}

TEST(SimulatorFCV1Plus, SaveLoad1CreateStorage)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    simulator->SetStones(SaveLoadInitialStones());

    auto storage = simulator->CreateStorage();
    auto const saved_raw =
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting();
    for (int i = 0; i < 10; ++i) {
        simulator->Step();
    }
    auto const expected =
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting();

    simulator->Load(*storage);
    EXPECT_EQ(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting(),
        saved_raw);
    for (int i = 0; i < 10; ++i) {
        simulator->Step();
    }
    EXPECT_EQ(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting(),
        expected);
}

TEST(SimulatorFCV1Plus, SaveLoad2SaveIntoExistingStorage)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    auto storage = simulator->CreateStorage();
    simulator->SetStones(SaveLoadInitialStones());
    simulator->Save(*storage);
    auto const saved_raw =
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting();
    simulator->Step();
    simulator->Load(*storage);
    EXPECT_EQ(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting(),
        saved_raw);
}

TEST(SimulatorFCV1Plus, SaveLoad3StorageCreatesSimulator)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    simulator->SetStones(SaveLoadInitialStones());
    auto storage = simulator->CreateStorage();
    auto copy = dct::AsStepSimulator(storage->CreateSimulator());
    EXPECT_EQ(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*copy)
            .GetRawStonesForTesting(),
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting());
}

TEST(SimulatorFCV1Plus, SaveLoad4JsonRoundTrip)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    simulator->SetStones(SaveLoadInitialStones());
    auto storage = simulator->CreateStorage();
    auto const& concrete =
        dynamic_cast<dcs::SimulatorFCV1PlusStorage const&>(*storage);
    nlohmann::json const json = concrete;
    auto restored_storage =
        json.get<dcs::SimulatorFCV1PlusStorage>();
    auto restored = dct::AsStepSimulator(restored_storage.CreateSimulator());
    EXPECT_EQ(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*restored)
            .GetRawStonesForTesting(),
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting());

    auto legacy_json = json;
    legacy_json["factory"].erase("seconds_per_frame");
    auto restored_without_timestep =
        dct::AsStepSimulator(
            legacy_json.get<dcs::SimulatorFCV1PlusStorage>().CreateSimulator());
    EXPECT_FLOAT_EQ(restored_without_timestep->GetSecondsPerFrame(), 0.001f);

    legacy_json["factory"]["seconds_per_frame"] = 0.002f;
    auto restored_with_legacy_timestep =
        dct::AsStepSimulator(
            legacy_json.get<dcs::SimulatorFCV1PlusStorage>().CreateSimulator());
    EXPECT_FLOAT_EQ(restored_with_legacy_timestep->GetSecondsPerFrame(), 0.001f);
}

TEST(SimulatorFCV1Plus, FactoryToJson)
{
    dcs::SimulatorFCV1PlusFactory factory;
    nlohmann::json const json = factory;
    EXPECT_EQ(json.at("type").get<std::string>(), "fcv1plus");
    EXPECT_EQ(json.at("seconds_per_frame").get<float>(), 0.001f);
}

TEST(SimulatorFCV1Plus, FactoryFromJsonUsesFixedTimestep)
{
    nlohmann::json const minimal = {{"type", "fcv1plus"}};
    auto factory = minimal.get<dcs::SimulatorFCV1PlusFactory>();
    EXPECT_FLOAT_EQ(dct::AsStepSimulator(factory.CreateSimulator())->GetSecondsPerFrame(), 0.001f);

    for (nlohmann::json const& legacy : {
             nlohmann::json(0.001f), nlohmann::json(0.002f),
             nlohmann::json("ignored"), nlohmann::json(nullptr)}) {
        auto input = minimal;
        input["seconds_per_frame"] = legacy;
        input.get_to(factory);
        EXPECT_FLOAT_EQ(dct::AsStepSimulator(factory.CreateSimulator())->GetSecondsPerFrame(), 0.001f);
    }

    EXPECT_THROW(
        nlohmann::json::array().get<dcs::SimulatorFCV1PlusFactory>(),
        nlohmann::json::type_error);
}

TEST(SimulatorFCV1PlusCollision, HeadOnConservesMomentumAndEnergyBothModes)
{
    EXPECT_TRUE(solver::kApplyFrictionTorque);
    for (bool const torque : {false, true}) {
        fcp::AllStonesFixed stones;
        stones[0] = MakeFixedStone(-dc::Stone::kRadius, 0.0f, 1.0f, 0.0f);
        stones[1] = MakeFixedStone(dc::Stone::kRadius, 0.0f, -1.0f, 0.0f);
        auto const before_energy = TotalUnitMassEnergy(stones);
        auto const before_momentum =
            static_cast<double>(stones[0].velocity.x) +
            static_cast<double>(stones[1].velocity.x);

        auto const collisions =
            solver::ResolveForTesting(stones, torque);
        ASSERT_EQ(collisions.size(), 1u);
        auto const after_momentum =
            static_cast<double>(stones[0].velocity.x) +
            static_cast<double>(stones[1].velocity.x);
        EXPECT_NEAR(
            before_momentum,
            after_momentum,
            kCollisionConservationTolerance);
        EXPECT_NEAR(
            before_energy,
            TotalUnitMassEnergy(stones),
            kCollisionConservationTolerance);
        EXPECT_NEAR(static_cast<double>(stones[0].velocity.x), -1.0,
                    kCollisionConservationTolerance);
        EXPECT_NEAR(static_cast<double>(stones[1].velocity.x), 1.0,
                    kCollisionConservationTolerance);
    }
}

TEST(SimulatorFCV1PlusCollision, ModeBObliqueSpinContactConservesAngularMomentum)
{
    fcp::AllStonesFixed stones;
    stones[0] = MakeFixedStone(
        -dc::Stone::kRadius, 0.0f, 1.0f, 1.0f, 1.5f);
    stones[1] = MakeFixedStone(
        dc::Stone::kRadius, 0.0f, -1.0f, -0.5f, -0.5f);

    auto const before_energy = TotalUnitMassEnergy(stones);
    auto const before_angular_momentum = TotalUnitMassAngularMomentum(stones);
    auto const before_momentum_x =
        static_cast<double>(stones[0].velocity.x) +
        static_cast<double>(stones[1].velocity.x);
    auto const before_momentum_y =
        static_cast<double>(stones[0].velocity.y) +
        static_cast<double>(stones[1].velocity.y);
    auto const collisions = solver::Resolve(stones);

    ASSERT_EQ(collisions.size(), 1u);
    // n=(1,0), t=(0,1), vn=-2 and
    // vt=(vB-vA)·t-r(wA+wB)=-1.5-0.145=-1.645.  The friction limit
    // is 0.4, so the unconstrained Mode B result qt=1.645/6 applies.
    constexpr double expected_normal_impulse = 2.0;
    constexpr double expected_tangent_impulse = 1.645 / 6.0;
    auto const radius = static_cast<double>(dc::Stone::kRadius);
    auto const expected_angular_delta =
        2.0 * expected_tangent_impulse / radius;
    EXPECT_NEAR(
        static_cast<double>(collisions[0].normal_velocity_impulse),
        expected_normal_impulse,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(collisions[0].tangent_velocity_impulse),
        expected_tangent_impulse,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.x), -1.0,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[1].velocity.x), 1.0,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.y),
        1.0 - expected_tangent_impulse,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[1].velocity.y),
        -0.5 + expected_tangent_impulse,
        kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].angular_velocity),
        1.5 - expected_angular_delta,
        kModeBAngularVelocityTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[1].angular_velocity),
        -0.5 - expected_angular_delta,
        kModeBAngularVelocityTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.x) +
        static_cast<double>(stones[1].velocity.x),
        before_momentum_x,
        kCollisionConservationTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.y) +
        static_cast<double>(stones[1].velocity.y),
        before_momentum_y,
        kCollisionConservationTolerance);
    EXPECT_NEAR(
        TotalUnitMassAngularMomentum(stones),
        before_angular_momentum,
        kCollisionConservationTolerance);
    EXPECT_LE(
        TotalUnitMassEnergy(stones),
        before_energy + kCollisionConservationTolerance);
}

TEST(SimulatorFCV1PlusCollision, ModeBFrictionImpulseHonorsCoulombLimit)
{
    fcp::AllStonesFixed stones;
    stones[0] = MakeFixedStone(-dc::Stone::kRadius, 0.0f, 1.0f, 4.0f);
    stones[1] = MakeFixedStone(dc::Stone::kRadius, 0.0f, -1.0f, -4.0f);
    auto const before_energy = TotalUnitMassEnergy(stones);
    auto const before_angular_momentum = TotalUnitMassAngularMomentum(stones);
    auto const before_momentum_x =
        static_cast<double>(stones[0].velocity.x) +
        static_cast<double>(stones[1].velocity.x);
    auto const before_momentum_y =
        static_cast<double>(stones[0].velocity.y) +
        static_cast<double>(stones[1].velocity.y);

    auto const collisions = solver::Resolve(stones);

    ASSERT_EQ(collisions.size(), 1u);
    auto const normal = static_cast<double>(collisions[0].normal_velocity_impulse);
    auto const tangent = static_cast<double>(collisions[0].tangent_velocity_impulse);
    EXPECT_NEAR(std::abs(tangent), 0.2 * normal, kModeBImpulseTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.x) +
        static_cast<double>(stones[1].velocity.x),
        before_momentum_x,
        kCollisionConservationTolerance);
    EXPECT_NEAR(
        static_cast<double>(stones[0].velocity.y) +
        static_cast<double>(stones[1].velocity.y),
        before_momentum_y,
        kCollisionConservationTolerance);
    EXPECT_NEAR(
        TotalUnitMassAngularMomentum(stones),
        before_angular_momentum,
        kModeBImpulseTolerance);
    EXPECT_LE(
        TotalUnitMassEnergy(stones),
        before_energy + kCollisionConservationTolerance);
}

TEST(SimulatorFCV1PlusCollision, SparseEnabledIndicesPreserveOriginalPairIdentityAndOrder)
{
    fcp::AllStonesFixed stones;
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);

    stones[0] = MakeFixedStone(0.0f, 0.0f, 1.0f, 0.0f);
    stones[2] = MakeFixedStone(0.0f, 0.0f, -1.0f, 0.0f);
    stones[2].position.x =
        fcp::Fixed::from_raw_value(diameter.raw_value());

    stones[5] = MakeFixedStone(0.0f, 2.0f, 2.0f, 0.0f);
    stones[9] = MakeFixedStone(0.0f, 2.0f, -1.0f, 0.0f);
    stones[9].position.x =
        fcp::Fixed::from_raw_value(diameter.raw_value());

    stones[13] = MakeFixedStone(0.0f, 4.0f);

    auto const collisions = solver::Resolve(stones);

    ASSERT_EQ(collisions.size(), 2u);
    EXPECT_EQ(collisions[0].a, 0);
    EXPECT_EQ(collisions[0].b, 2);
    EXPECT_EQ(
        collisions[0].normal_velocity_impulse.raw_value(),
        fcp::WideFixed(2).raw_value());
    EXPECT_EQ(collisions[0].tangent_velocity_impulse.raw_value(), 0);
    EXPECT_EQ(collisions[1].a, 5);
    EXPECT_EQ(collisions[1].b, 9);
    EXPECT_EQ(
        collisions[1].normal_velocity_impulse.raw_value(),
        fcp::WideFixed(3).raw_value());
    EXPECT_EQ(collisions[1].tangent_velocity_impulse.raw_value(), 0);

    for (auto const& collision : collisions) {
        EXPECT_TRUE(stones[collision.a].enabled);
        EXPECT_TRUE(stones[collision.b].enabled);
    }
}

TEST(SimulatorFCV1PlusCollision, TriangularAccumulatorKeepsDistinctPairImpulses)
{
    fcp::AllStonesFixed stones;
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);

    stones[0] = MakeFixedStone(0.0f, 0.0f, 3.0f, 8.0f);
    stones[1] = MakeFixedStone(0.0f, 0.0f, 0.0f, 8.0f);
    stones[1].position.x =
        fcp::Fixed::from_raw_value(diameter.raw_value());
    stones[2] = MakeFixedStone(0.0f, 0.0f, 0.0f, 4.0f);
    stones[2].position.y =
        fcp::Fixed::from_raw_value(diameter.raw_value());
    stones[3] = MakeFixedStone(0.0f, 0.0f, 1.0f, 4.0f);
    stones[3].position.x =
        fcp::Fixed::from_raw_value(-diameter.raw_value());
    stones[4] = MakeFixedStone(0.0f, 0.0f, 1.0f, 6.0f);
    stones[4].position.y =
        fcp::Fixed::from_raw_value(-diameter.raw_value());

    auto const collisions = solver::Resolve(stones);

    std::array<std::uint8_t, 4> const expected_b = {1, 2, 3, 4};
    std::array<int, 4> const expected_normal_impulses = {3, 4, 1, 2};
    ASSERT_EQ(collisions.size(), expected_b.size());
    for (std::size_t index = 0; index < expected_b.size(); ++index) {
        EXPECT_EQ(collisions[index].a, 0);
        EXPECT_EQ(collisions[index].b, expected_b[index]);
        EXPECT_EQ(
            collisions[index].normal_velocity_impulse.raw_value(),
            fcp::WideFixed(expected_normal_impulses[index]).raw_value());
        EXPECT_NE(collisions[index].normal_velocity_impulse.raw_value(), 0);
        EXPECT_EQ(collisions[index].tangent_velocity_impulse.raw_value(), 0);
    }
    for (std::size_t first = 0; first < collisions.size(); ++first) {
        for (std::size_t second = first + 1;
             second < collisions.size();
             ++second)
        {
            EXPECT_NE(
                collisions[first].normal_velocity_impulse.raw_value(),
                collisions[second].normal_velocity_impulse.raw_value());
        }
    }
}

TEST(SimulatorFCV1PlusCollision, SixteenStoneStressStateStaysInRange)
{
    fcp::AllStonesFixed stones;
    for (int i = 0; i < 16; ++i) {
        auto const row = i / 4;
        auto const column = i % 4;
        stones[i] = MakeFixedStone(
            (column - 1.5f) * 0.28f,
            (row - 1.5f) * 0.28f,
            (column - 1.5f) * 0.2f,
            (row - 1.5f) * 0.2f);
    }
    EXPECT_NO_THROW(solver::Resolve(stones));
    for (auto const& stone : stones) {
        EXPECT_NO_THROW(fcp::ValidateRuntimeState(stone));
    }
}

TEST(SimulatorFCV1PlusCollision, RepeatedLargePairImpulsesAccumulateInWideFixed)
{
    fcp::AllStonesFixed stones;
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);
    std::array<float, 4> const velocities = {
        fcp::kRuntimeSpeed - 0.5f,
        0.0f,
        -fcp::kRuntimeSpeed + 0.5f,
        -fcp::kRuntimeSpeed + 0.5f
    };
    for (int index = 0; index < 4; ++index) {
        auto& stone = stones[index];
        stone.enabled = true;
        stone.position.x = fcp::Fixed::from_raw_value(
            diameter.raw_value() * index);
        stone.velocity.x = fcp::Fixed(velocities[index]);
    }

    std::vector<solver::CollisionResult> collisions;
    EXPECT_NO_THROW(collisions = solver::Resolve(stones));

    auto const pair = std::find_if(
        collisions.begin(),
        collisions.end(),
        [](solver::CollisionResult const& value) {
            return value.a == 1 && value.b == 2;
        });
    ASSERT_NE(pair, collisions.end());
    auto const normal =
        static_cast<double>(pair->normal_velocity_impulse);
    EXPECT_TRUE(std::isfinite(normal));
    EXPECT_GT(normal, 128.0);
    EXPECT_TRUE(std::isfinite(
        static_cast<double>(pair->tangent_velocity_impulse)));
}

TEST(SimulatorFCV1PlusCollision, TangentialFrictionDoesNotAddEnergy)
{
    for (bool const torque : {false, true}) {
        fcp::AllStonesFixed stones;
        stones[0] = MakeFixedStone(-dc::Stone::kRadius, 0.0f, 1.0f, 0.5f);
        stones[1] = MakeFixedStone(dc::Stone::kRadius, 0.0f, -1.0f, -0.5f);
        auto const before = TotalUnitMassEnergy(stones);
        solver::ResolveForTesting(stones, torque);
        EXPECT_LE(
            TotalUnitMassEnergy(stones),
            before + kCollisionConservationTolerance);
    }
}

TEST(SimulatorFCV1PlusCollision, FrictionTorqueDivisionIsCheckedBeforeNarrowing)
{
    fcp::AllStonesFixed in_range;
    in_range[0] = MakeFixedStone(
        -dc::Stone::kRadius, 0.0f, 17.0f, 44.5f);
    in_range[1] = MakeFixedStone(
        dc::Stone::kRadius, 0.0f, -17.0f, -44.5f);
    EXPECT_NO_THROW(solver::ResolveForTesting(in_range, true));
    for (int i = 0; i < 2; ++i) {
        auto const omega =
            static_cast<double>(in_range[i].angular_velocity);
        EXPECT_TRUE(std::isfinite(omega));
        EXPECT_LE(std::abs(omega), fcp::kRuntimeAngularVelocity);
    }

    fcp::AllStonesFixed out_of_range;
    // The true angular delta is about -165.5. The old Q8.24 division wrapped
    // that to about +90.5, which stayed inside the separate +/-96 runtime
    // guard and therefore exposed the missing narrowing check.
    out_of_range[0] = MakeFixedStone(
        -dc::Stone::kRadius, 0.0f, 30.0f, -37.0f);
    out_of_range[1] = MakeFixedStone(
        dc::Stone::kRadius, 0.0f, -30.0f, 37.0f);
    EXPECT_THROW(
        solver::ResolveForTesting(out_of_range, true),
        std::overflow_error);
}

TEST(SimulatorFCV1PlusCollision, TouchingAndSeparatingHasNoImpulse)
{
    fcp::AllStonesFixed stones;
    stones[0] = MakeFixedStone(-dc::Stone::kRadius, 0.0f, -1.0f, 0.0f);
    stones[1] = MakeFixedStone(dc::Stone::kRadius, 0.0f, 1.0f, 0.0f);
    auto const before = stones;
    auto const collisions = solver::Resolve(stones);
    EXPECT_TRUE(collisions.empty());
    EXPECT_EQ(stones[0].velocity.x.raw_value(),
              before[0].velocity.x.raw_value());
    EXPECT_EQ(stones[1].velocity.x.raw_value(),
              before[1].velocity.x.raw_value());
}

TEST(SimulatorFCV1PlusCollision, CoincidentCentersUseDeterministicFallback)
{
    fcp::AllStonesFixed a;
    a[0] = MakeFixedStone(0.0f, 0.0f, 1.0f, 0.0f);
    a[1] = MakeFixedStone(0.0f, 0.0f, -1.0f, 0.0f);
    auto b = a;
    EXPECT_NO_THROW(solver::Resolve(a));
    EXPECT_NO_THROW(solver::Resolve(b));
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(a[i].position.x.raw_value(), b[i].position.x.raw_value());
        EXPECT_EQ(a[i].position.y.raw_value(), b[i].position.y.raw_value());
        EXPECT_EQ(a[i].velocity.x.raw_value(), b[i].velocity.x.raw_value());
        EXPECT_EQ(a[i].velocity.y.raw_value(), b[i].velocity.y.raw_value());
    }
}

TEST(SimulatorFCV1PlusCollision, FullyStationaryCoincidentCentersUseFixedAxisFallback)
{
    // Both position AND relative velocity are exactly zero: the design's
    // innermost fallback (fixed axis (1,0)) must apply, not the
    // velocity-direction fallback used above. There is no velocity
    // response (relative normal velocity is exactly zero, not negative),
    // only penetration correction along the fallback normal, which makes
    // the (1,0) axis directly observable via the resulting positions.
    fcp::AllStonesFixed a;
    a[0] = MakeFixedStone(0.0f, 0.0f, 0.0f, 0.0f);
    a[1] = MakeFixedStone(0.0f, 0.0f, 0.0f, 0.0f);
    auto b = a;
    EXPECT_NO_THROW(solver::Resolve(a));
    EXPECT_NO_THROW(solver::Resolve(b));

    EXPECT_LT(static_cast<double>(a[0].position.x), 0.0);
    EXPECT_GT(static_cast<double>(a[1].position.x), 0.0);
    EXPECT_EQ(static_cast<double>(a[0].position.y), 0.0);
    EXPECT_EQ(static_cast<double>(a[1].position.y), 0.0);
    EXPECT_NEAR(
        static_cast<double>(a[0].position.x),
        -static_cast<double>(a[1].position.x),
        kCollisionConservationTolerance);

    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(a[i].position.x.raw_value(), b[i].position.x.raw_value());
        EXPECT_EQ(a[i].position.y.raw_value(), b[i].position.y.raw_value());
        EXPECT_EQ(a[i].velocity.x.raw_value(), b[i].velocity.x.raw_value());
        EXPECT_EQ(a[i].velocity.y.raw_value(), b[i].velocity.y.raw_value());
    }
}

TEST(SimulatorFCV1PlusCollision, ThreeAndFourStoneChainsConvergeDeterministically)
{
    for (int count : {3, 4}) {
        fcp::AllStonesFixed first;
        auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);
        for (int i = 0; i < count; ++i) {
            first[i] = MakeFixedStone(
                static_cast<float>(diameter * i),
                0.0f,
                i == 0 ? 1.0f : 0.0f,
                0.0f);
        }
        auto second = first;
        solver::Resolve(first);
        solver::Resolve(second);
        for (int i = 0; i < count; ++i) {
            EXPECT_EQ(first[i].position.x.raw_value(),
                      second[i].position.x.raw_value());
            EXPECT_EQ(first[i].velocity.x.raw_value(),
                      second[i].velocity.x.raw_value());
        }
        for (int i = 1; i < count; ++i) {
            auto const gap =
                static_cast<std::int64_t>(first[i].position.x.raw_value()) -
                static_cast<std::int64_t>(first[i - 1].position.x.raw_value());
            EXPECT_GE(gap, diameter.raw_value() - 1);
        }
    }
}

TEST(SimulatorFCV1PlusCollision, StationaryTouchingChainExitsAfterOnePass)
{
    fcp::AllStonesFixed stones;
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);
    for (int index = 0; index < 4; ++index) {
        auto& stone = stones[index];
        stone.enabled = true;
        stone.position.x = fcp::Fixed::from_raw_value(
            diameter.raw_value() * index);
    }

    int passes_used = 0;
    auto const collisions =
        solver::ResolveForTesting(stones, false, &passes_used);

    EXPECT_TRUE(collisions.empty());
    EXPECT_EQ(passes_used, 1);
}

TEST(SimulatorFCV1PlusCollision, ReverseOrderedSixteenStoneChainFullyResolves)
{
    fcp::AllStonesFixed stones;
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);
    for (int index = 0; index < dc::StoneCoordinate::kStoneMax; ++index) {
        auto const spatial_position =
            dc::StoneCoordinate::kStoneMax - 1 - index;
        auto& stone = stones[index];
        stone.enabled = true;
        stone.position.y = fcp::Fixed::from_raw_value(
            diameter.raw_value() * spatial_position);
        stone.velocity.y = fcp::Fixed(
            index == dc::StoneCoordinate::kStoneMax - 1 ? 1.0f : 0.0f);
    }

    EXPECT_NO_THROW(solver::Resolve(stones));

    for (int left = dc::StoneCoordinate::kStoneMax - 1; left > 0; --left) {
        auto const right = left - 1;
        auto const gap =
            static_cast<std::int64_t>(stones[right].position.y.raw_value()) -
            static_cast<std::int64_t>(stones[left].position.y.raw_value());
        auto const relative_normal_velocity =
            static_cast<std::int64_t>(stones[right].velocity.y.raw_value()) -
            static_cast<std::int64_t>(stones[left].velocity.y.raw_value());
        EXPECT_GE(gap, diameter.raw_value() - 1);
        EXPECT_GE(relative_normal_velocity, -1);
    }
}

TEST(SimulatorFCV1Plus, HighSpeedStepDoesNotTunnel)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones stones;
    stones[0].emplace(
        dc::Vector2(-0.147f, 10.0f),
        0.0f,
        dc::Vector2(5.0f, 0.0f),
        1.0f);
    stones[1].emplace(
        dc::Vector2(0.147f, 10.0f),
        0.0f,
        dc::Vector2(-5.0f, 0.0f),
        -1.0f);
    simulator->SetStones(stones);
    simulator->Step();
    auto const result = simulator->GetStones();
    EXPECT_LT(result[0]->translational_velocity.x, 0.0f);
    EXPECT_GT(result[1]->translational_velocity.x, 0.0f);
}

TEST(SimulatorFCV1Plus, DistinctContactEntryOrderIgnoresStoneIds)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto run = [&](bool swap_targets) {
        auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
        dcs::ISimulator::AllStones stones;
        // Stone A moves 5 mm in this frame. C enters 2.1 mm before the end;
        // B enters 0.5 mm before the end. B has the lower ID in the first
        // run even though it is farther along A's direction of travel.
        stones[0].emplace(
            dc::Vector2(-0.005f, 10.0f), 0.0f,
            dc::Vector2(5.0f, 0.0f), 0.0f);
        auto const b = dcs::ISimulator::StoneState(
            dc::Vector2(0.2895f, 10.0f), 0.0f, dc::Vector2(), 0.0f);
        auto const c = dcs::ISimulator::StoneState(
            dc::Vector2(0.1400f, 10.2528f), 0.0f, dc::Vector2(), 0.0f);
        stones[swap_targets ? 2 : 1] = b;
        stones[swap_targets ? 1 : 2] = c;
        simulator->SetStones(stones);
        simulator->Step();
        return dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
            .GetRawStonesForTesting();
    };

    auto const low_id_late = run(false);
    auto const high_id_late = run(true);

    // This is the Q8.24 outcome of resolving geometrically earlier C (ID 2)
    // before later B (ID 1). It binds the test to chronology, rather than
    // merely requiring the two ID permutations to agree with each other.
    EXPECT_EQ(
        low_id_late[0],
        (dcs::SimulatorFCV1PlusRawStone{
            true, -6271, 167764651, 0, 44981, -29127764, 147613952}));
    EXPECT_EQ(
        low_id_late[1],
        (dcs::SimulatorFCV1PlusRawStone{
            true, 4859116, 167772163, 0, 57041086, -2486385, 35506944}));
    EXPECT_EQ(
        low_id_late[2],
        (dcs::SimulatorFCV1PlusRawStone{
            true, 2352967, 172020946, 0, 26798917, 31614149, 112107008}));

    EXPECT_EQ(low_id_late[0], high_id_late[0]);
    EXPECT_EQ(low_id_late[1], high_id_late[2]);
    EXPECT_EQ(low_id_late[2], high_id_late[1]);
}

TEST(SimulatorFCV1PlusCollision, AlreadyTouchingChainsConvergeInBothDirections)
{
    auto const diameter = fcp::Fixed(dc::Stone::kRadius * 2.0f);
    for (int count : {3, 4}) {
        // Forward direction: stone 0 moves into stone 1 .. count-1
        {
            fcp::AllStonesFixed stones;
            solver::StartPositions start_positions{};
            for (int i = 0; i < count; ++i) {
                stones[i] = MakeFixedStone(
                    static_cast<float>(diameter * i),
                    0.0f,
                    i == 0 ? 1.0f : 0.0f,
                    0.0f);
                start_positions[i] = stones[i].position;
            }
            // Stone 0 enters a chain that is already touching at frame start.
            start_positions[0].x -= fcp::Fixed(0.001f);
            solver::Resolve(stones, &start_positions);
            for (int i = 1; i < count; ++i) {
                auto const gap =
                    static_cast<std::int64_t>(stones[i].position.x.raw_value()) -
                    static_cast<std::int64_t>(stones[i - 1].position.x.raw_value());
                auto const rel_v =
                    static_cast<std::int64_t>(stones[i].velocity.x.raw_value()) -
                    static_cast<std::int64_t>(stones[i - 1].velocity.x.raw_value());
                EXPECT_GE(gap, diameter.raw_value() - 1);
                if (gap < diameter.raw_value()) {
                    EXPECT_GE(rel_v, -1);
                }
            }
        }
        // Reverse direction: stone count-1 moves into stone count-2 .. 0
        {
            fcp::AllStonesFixed stones;
            solver::StartPositions start_positions{};
            for (int i = 0; i < count; ++i) {
                stones[i] = MakeFixedStone(
                    static_cast<float>(diameter * i),
                    0.0f,
                    i == count - 1 ? -1.0f : 0.0f,
                    0.0f);
                start_positions[i] = stones[i].position;
            }
            // The reverse-indexed endpoint enters the same touching chain.
            start_positions[count - 1].x += fcp::Fixed(0.001f);
            solver::Resolve(stones, &start_positions);
            for (int i = 1; i < count; ++i) {
                auto const gap =
                    static_cast<std::int64_t>(stones[i].position.x.raw_value()) -
                    static_cast<std::int64_t>(stones[i - 1].position.x.raw_value());
                auto const rel_v =
                    static_cast<std::int64_t>(stones[i].velocity.x.raw_value()) -
                    static_cast<std::int64_t>(stones[i - 1].velocity.x.raw_value());
                EXPECT_GE(gap, diameter.raw_value() - 1);
                if (gap < diameter.raw_value()) {
                    EXPECT_GE(rel_v, -1);
                }
            }
        }
    }
}

TEST(SimulatorFCV1Plus, SplitContactEqualKeyRepeatabilityAndBoundedEnergy)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto run = [&]() {
        auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
        dcs::ISimulator::AllStones stones;
        // Central stationary stone
        stones[0].emplace(
            dc::Vector2(0.0f, 10.0f), 0.0f, dc::Vector2(), 0.0f);
        // Symmetrical left and right approaching stones entering at identical times
        stones[1].emplace(
            dc::Vector2(-0.291f, 10.0f), 0.0f,
            dc::Vector2(2.0f, 0.0f), 0.0f);
        stones[2].emplace(
            dc::Vector2(0.291f, 10.0f), 0.0f,
            dc::Vector2(-2.0f, 0.0f), 0.0f);
        simulator->SetStones(stones);

        fcp::AllStonesFixed before;
        for (int i = 0; i < 3; ++i) {
            before[i] = MakeFixedStone(
                stones[i]->position.x,
                stones[i]->position.y,
                stones[i]->translational_velocity.x,
                stones[i]->translational_velocity.y,
                stones[i]->angular_velocity);
        }
        auto const before_energy = TotalUnitMassEnergy(before);

        simulator->Step();

        auto const raw =
            dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator)
                .GetRawStonesForTesting();

        fcp::AllStonesFixed after;
        for (int i = 0; i < 3; ++i) {
            after[i].enabled = raw[i].enabled;
            after[i].position = {
                fcp::Fixed::from_raw_value(raw[i].position_x),
                fcp::Fixed::from_raw_value(raw[i].position_y)};
            after[i].velocity = {
                fcp::Fixed::from_raw_value(raw[i].velocity_x),
                fcp::Fixed::from_raw_value(raw[i].velocity_y)};
            after[i].angular_velocity =
                fcp::Fixed::from_raw_value(raw[i].angular_velocity);
        }
        auto const after_energy = TotalUnitMassEnergy(after);
        EXPECT_LE(after_energy, before_energy + kCollisionConservationTolerance);

        return raw;
    };

    auto const first_run = run();
    auto const second_run = run();
    EXPECT_EQ(first_run, second_run);
}

TEST(SimulatorFCV1Plus, DeterministicRawStateAndGoldenHash)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto first = dct::AsStepSimulator(factory.CreateSimulator());
    auto second = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones stones;
    stones[0].emplace(
        dc::Vector2(-0.15f, 10.0f),
        0.0f,
        dc::Vector2(0.08f, 1.5f),
        1.0f);
    stones[1].emplace(
        dc::Vector2(0.15f, 10.0f),
        0.0f,
        dc::Vector2(-0.02f, 1.2f),
        -0.5f);
    first->SetStones(stones);
    second->SetStones(stones);
    for (int frame = 0; frame < 5000; ++frame) {
        first->Step();
        second->Step();
        EXPECT_EQ(
            dynamic_cast<dcs::SimulatorFCV1Plus&>(*first)
                .GetRawStonesForTesting(),
            dynamic_cast<dcs::SimulatorFCV1Plus&>(*second)
                .GetRawStonesForTesting());
    }
    auto const hash = HashRawState(
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*first)
            .GetRawStonesForTesting());
    // Updated for the small-angle sliding sine revision. This exact Mode B
    // scenario produced the same raw-state hash in compiled MSVC, GCC, and
    // Clang runs. The regression value is independent of native struct layout
    // and JSON formatting by construction (see HashRawState above).
    // Angular damping revision verified in compiled MSVC, GCC, and Clang runs.
    constexpr std::uint64_t kExpectedGoldenHash = 0x3e4dac5550b588abull;
    EXPECT_EQ(hash, kExpectedGoldenHash);
}

TEST(SimulatorFCV1Plus, RejectsInvalidInputs)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    dcs::ISimulator::AllStones stones;
    stones[0].emplace(
        dc::Vector2(std::numeric_limits<float>::quiet_NaN(), 0.0f),
        0.0f,
        dc::Vector2(),
        0.0f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);

    stones[0].emplace(
        dc::Vector2(10.01f, 0.0f),
        0.0f,
        dc::Vector2(),
        0.0f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);

    stones[0].emplace(
        dc::Vector2(),
        0.0f,
        dc::Vector2(10.01f, 0.0f),
        0.0f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);

    stones[0].emplace(
        dc::Vector2(0.0f, 50.01f),
        0.0f,
        dc::Vector2(),
        0.0f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);

    stones[0].emplace(
        dc::Vector2(),
        std::numeric_limits<float>::infinity(),
        dc::Vector2(),
        0.0f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);

    stones[0].emplace(
        dc::Vector2(),
        0.0f,
        dc::Vector2(),
        20.01f);
    EXPECT_THROW(simulator->SetStones(stones), std::invalid_argument);
}

TEST(SimulatorFCV1Plus, SetStonesPositionBoundariesUseTightAsymmetricRange)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    auto set_position = [&](float x, float y) {
        dcs::ISimulator::AllStones stones;
        stones[0].emplace(
            dc::Vector2(x, y),
            0.0f,
            dc::Vector2(),
            0.0f);
        simulator->SetStones(stones);
    };

    constexpr float kXLimit =
        dc::coordinate::kSheetWidthMax / 2.0f - dc::Stone::kRadius;
    constexpr float kYLimit =
        dc::coordinate::kBackBoardY - dc::Stone::kRadius;
    auto const positive_x_inside = std::nextafter(kXLimit, 0.0f);
    auto const positive_x_outside =
        std::nextafter(kXLimit, std::numeric_limits<float>::infinity());
    auto const negative_x_inside = std::nextafter(-kXLimit, 0.0f);
    auto const negative_x_outside =
        std::nextafter(-kXLimit, -std::numeric_limits<float>::infinity());
    auto const positive_y_inside = std::nextafter(kYLimit, 0.0f);
    auto const positive_y_outside =
        std::nextafter(kYLimit, std::numeric_limits<float>::infinity());
    auto const negative_y_outside =
        std::nextafter(0.0f, -std::numeric_limits<float>::infinity());

    EXPECT_NO_THROW(set_position(positive_x_inside, 1.0f));
    EXPECT_THROW(set_position(kXLimit, 1.0f), std::invalid_argument);
    EXPECT_THROW(set_position(positive_x_outside, 1.0f), std::invalid_argument);
    EXPECT_NO_THROW(set_position(negative_x_inside, 1.0f));
    EXPECT_THROW(set_position(-kXLimit, 1.0f), std::invalid_argument);
    EXPECT_THROW(set_position(negative_x_outside, 1.0f), std::invalid_argument);

    EXPECT_NO_THROW(set_position(0.0f, positive_y_inside));
    EXPECT_THROW(set_position(0.0f, kYLimit), std::invalid_argument);
    EXPECT_THROW(set_position(0.0f, positive_y_outside), std::invalid_argument);
    EXPECT_NO_THROW(set_position(0.0f, std::nextafter(0.0f, 1.0f)));
    EXPECT_NO_THROW(set_position(0.0f, 0.0f));
    EXPECT_THROW(set_position(0.0f, negative_y_outside), std::invalid_argument);
}

TEST(SimulatorFCV1Plus, SetStoneUsesTightPositionValidation)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
    std::optional<dcs::ISimulator::StoneState> stone;
    stone.emplace(dc::Vector2(0.0f, 0.0f), 0.0f, dc::Vector2(), 0.0f);
    EXPECT_NO_THROW(simulator->SetStone(dc::StoneIndex(dc::Team::k0, 0), stone));

    stone.emplace(dc::Vector2(0.0f, -1.0f), 0.0f, dc::Vector2(), 0.0f);
    EXPECT_THROW(
        simulator->SetStone(dc::StoneIndex(dc::Team::k0, 0), stone),
        std::invalid_argument);
}

TEST(SimulatorFCV1Plus, RejectsMalformedRawStorage)
{
    dcs::SimulatorFCV1PlusStorage storage;
    auto json = storage.ToJson();
    json["schema_version"] = 2;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::invalid_argument);

    json = storage.ToJson();
    json["stones"].erase(json["stones"].begin());
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::invalid_argument);

    json = storage.ToJson();
    json["stones"][0]["enabled"] = false;
    json["stones"][0]["position_x"] = 1;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::invalid_argument);
}

TEST(SimulatorFCV1Plus, RejectsUnsignedRawValuesOutsideInt32Range)
{
    dcs::SimulatorFCV1PlusStorage storage;

    auto json = storage.ToJson();
    json["stones"][0]["position_x"] =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max()) + 1;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::out_of_range);

    json = storage.ToJson();
    json["stones"][0]["position_x"] =
        std::numeric_limits<std::uint64_t>::max();
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::out_of_range);
}

TEST(SimulatorFCV1Plus, FailedLoadLeavesOriginalStateUnchanged)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1Plus simulator(factory);
    dcs::ISimulator::AllStones initial;
    initial[3].emplace(
        dc::Vector2(0.25f, 12.0f),
        0.75f,
        dc::Vector2(-0.5f, 1.25f),
        -2.0f);
    simulator.SetStones(initial);

    auto const original_raw = simulator.GetRawStonesForTesting();
    auto const original_stones = simulator.GetStones();
    auto const original_seconds_per_frame = simulator.GetSecondsPerFrame();

    dcs::SimulatorFCV1PlusStorage invalid_angle;
    simulator.Save(invalid_angle);
    invalid_angle.stones[7].enabled = true;
    invalid_angle.stones[7].angle = fcp::Fixed::pi().raw_value();
    EXPECT_THROW(simulator.Load(invalid_angle), std::invalid_argument);

    EXPECT_FLOAT_EQ(
        simulator.GetSecondsPerFrame(), original_seconds_per_frame);
    EXPECT_EQ(simulator.GetRawStonesForTesting(), original_raw);
    ASSERT_TRUE(simulator.GetStones()[3].has_value());
    EXPECT_FLOAT_EQ(
        simulator.GetStones()[3]->angle,
        original_stones[3]->angle);
    EXPECT_FLOAT_EQ(
        simulator.GetStones()[3]->position.x,
        original_stones[3]->position.x);
    EXPECT_FLOAT_EQ(
        simulator.GetStones()[3]->translational_velocity.y,
        original_stones[3]->translational_velocity.y);
}

TEST(SimulatorFCV1Plus, RejectsMalformedSchemaVersion)
{
    dcs::SimulatorFCV1PlusStorage storage;

    // Non-integer: nlohmann's get<uint32_t>() would otherwise silently
    // truncate this instead of rejecting it.
    auto json = storage.ToJson();
    json["schema_version"] = 1.5;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::invalid_argument);

    // Out of uint32_t range: get<uint32_t>() would otherwise silently wrap.
    json = storage.ToJson();
    json["schema_version"] =
        static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::out_of_range);

    // Negative.
    json = storage.ToJson();
    json["schema_version"] = -1;
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::out_of_range);

    // Non-numeric.
    json = storage.ToJson();
    json["schema_version"] = "1";
    EXPECT_THROW(
        json.get<dcs::SimulatorFCV1PlusStorage>(),
        std::invalid_argument);
}

TEST(SimulatorFCV1Plus, ValidateRuntimeStateRejectsExtremeRawValuesWithoutUB)
{
    // fpm::abs()/unary operator- negate the raw int32_t value directly and
    // are UB at INT32_MIN; ValidateRuntimeState must not call them. These
    // raw values are reachable via Load()/JSON, not just in-process.
    dcs::SimulatorFCV1PlusFactory factory;

    // Both axes use the unified 64 m runtime limit. INT32_MIN remains far
    // outside it and must be rejected without negating the raw int32 value.
    dcs::SimulatorFCV1PlusStorage position_x_storage(factory);
    position_x_storage.stones[0].enabled = true;
    position_x_storage.stones[0].position_x =
        std::numeric_limits<std::int32_t>::min();
    EXPECT_THROW(position_x_storage.CreateSimulator(), std::overflow_error);

    dcs::SimulatorFCV1PlusStorage position_y_storage(factory);
    position_y_storage.stones[0].enabled = true;
    position_y_storage.stones[0].position_y =
        std::numeric_limits<std::int32_t>::min();
    EXPECT_THROW(position_y_storage.CreateSimulator(), std::overflow_error);

    dcs::SimulatorFCV1PlusStorage angular_velocity_storage(factory);
    angular_velocity_storage.stones[0].enabled = true;
    angular_velocity_storage.stones[0].angular_velocity =
        std::numeric_limits<std::int32_t>::min();
    EXPECT_THROW(
        angular_velocity_storage.CreateSimulator(), std::overflow_error);
}

TEST(SimulatorFCV1Plus, SheetOutSweepKeepsStationaryRawEdgeBufferEnabled)
{
    dcs::SimulatorFCV1PlusFactory factory;
    std::array<dcs::SimulatorFCV1PlusRawStone, 7> const cases = {
        MakeRawStone(fcp::kSheetOutPositionX - 0.001f, 10.0f),
        MakeRawStone(-fcp::kSheetOutPositionX + 0.001f, 10.0f),
        MakeRawStone(0.0f, fcp::kSheetOutPositionY - 0.001f),
        MakeRawStone(fcp::kSheetOutPositionX, 10.0f),
        MakeRawStone(-fcp::kSheetOutPositionX, 10.0f),
        MakeRawStone(0.0f, fcp::kSheetOutPositionY),
        MakeRawStone(0.0f, 0.0f)
    };

    for (auto const& raw : cases) {
        dcs::SimulatorFCV1PlusStorage storage(factory);
        storage.stones[0] = raw;
        auto simulator = dct::AsStepSimulator(storage.CreateSimulator());
        simulator->Step();
        EXPECT_TRUE(simulator->GetStones()[0].has_value());
    }
}

TEST(SimulatorFCV1Plus, SheetOutSweepDisablesStoneCrossingEachRawSheetEdge)
{
    dcs::SimulatorFCV1PlusFactory factory;
    std::array<dcs::SimulatorFCV1PlusRawStone, 4> const cases = {
        MakeRawStone(fcp::kSheetOutPositionX - 0.005f, 10.0f, 10.0f, 0.0f),
        MakeRawStone(-fcp::kSheetOutPositionX + 0.005f, 10.0f, -10.0f, 0.0f),
        MakeRawStone(0.0f, fcp::kSheetOutPositionY - 0.005f, 0.0f, 10.0f),
        MakeRawStone(0.0f, 0.005f, 0.0f, -10.0f)
    };

    for (auto const& raw : cases) {
        dcs::SimulatorFCV1PlusStorage storage(factory);
        storage.stones[0] = raw;
        auto simulator = dct::AsStepSimulator(storage.CreateSimulator());
        EXPECT_NO_THROW(simulator->Step());
        EXPECT_FALSE(simulator->GetStones()[0].has_value());
    }
}

TEST(SimulatorFCV1Plus, SheetOutSweepDisablesAlreadyOutStationaryRawStone)
{
    dcs::SimulatorFCV1PlusFactory factory;
    std::array<dcs::SimulatorFCV1PlusRawStone, 4> const cases = {
        MakeRawStone(fcp::kSheetOutPositionX + 0.001f, 10.0f),
        MakeRawStone(-fcp::kSheetOutPositionX - 0.001f, 10.0f),
        MakeRawStone(0.0f, fcp::kSheetOutPositionY + 0.001f),
        MakeRawStone(0.0f, -0.001f)
    };

    for (auto const& raw : cases) {
        dcs::SimulatorFCV1PlusStorage storage(factory);
        storage.stones[0] = raw;
        auto simulator = dct::AsStepSimulator(storage.CreateSimulator());
        simulator->Step();
        EXPECT_FALSE(simulator->GetStones()[0].has_value());
    }
}

TEST(SimulatorFCV1Plus, CollisionCorrectionDisablesOnlyAfterResolution)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1PlusStorage storage(factory);
    auto const edge = fcp::kSheetOutPositionX;
    storage.stones[0] =
        MakeRawStone(edge - 0.281f, 10.0f, 0.1f, 0.0f);
    storage.stones[1] =
        MakeRawStone(edge - 0.001f, 10.0f, -0.1f, 0.0f);
    auto simulator = dct::AsStepSimulator(storage.CreateSimulator());

    simulator->Step();

    // The two stones collided: stone 0 was moving toward stone 1 at 0.1 m/s
    // and no longer does (friction alone cannot slow it that much).
    ASSERT_TRUE(simulator->GetStones()[0].has_value());
    EXPECT_LT(simulator->GetStones()[0]->translational_velocity.x, 0.05f);
    // Stone 1 was pushed beyond the raw sheet edge by the collision, so it is
    // disabled only after the resolution.
    EXPECT_FALSE(simulator->GetStones()[1].has_value());

    // The disabled stone participated in the first Resolve() but is omitted
    // from the enabled-index snapshot used by the next call.
    simulator->Step();
    EXPECT_TRUE(simulator->GetStones()[0].has_value());
    EXPECT_FALSE(simulator->GetStones()[1].has_value());
}

TEST(SimulatorFCV1Plus, ExactlyZeroStoneStateIsBitExactAcrossSteps)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1PlusStorage storage(factory);
    storage.stones[0] = MakeRawStone(1.0f, 10.0f);
    storage.stones[0].angle = fcp::Fixed(0.75f).raw_value();
    auto simulator = dct::AsStepSimulator(storage.CreateSimulator());
    auto& concrete = dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator);
    auto const before = concrete.GetRawStonesForTesting()[0];

    for (int i = 0; i < 8; ++i) {
        simulator->Step();
        EXPECT_EQ(concrete.GetRawStonesForTesting()[0], before);
    }
}

TEST(SimulatorFCV1Plus, ValidateRuntimeStateSpeedThresholdMatchesSqrtBasedComparison)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto const speed_limit = fcp::Fixed(fcp::kRuntimeSpeed);
    auto const speed_limit_raw =
        static_cast<std::uint64_t>(speed_limit.raw_value());
    auto const threshold =
        speed_limit_raw * speed_limit_raw + speed_limit_raw;

    fcp::FixedVector2 const accepted_velocity{
        fcp::Fixed::from_raw_value(speed_limit.raw_value()),
        fcp::Fixed::from_raw_value(28377)};
    fcp::FixedVector2 const rejected_velocity{
        fcp::Fixed::from_raw_value(speed_limit.raw_value()),
        fcp::Fixed::from_raw_value(28378)};

    EXPECT_LE(accepted_velocity.SquaredRawLength(), threshold);
    EXPECT_GT(rejected_velocity.SquaredRawLength(), threshold);
    EXPECT_LE(accepted_velocity.Length(), speed_limit);
    EXPECT_GT(rejected_velocity.Length(), speed_limit);

    dcs::SimulatorFCV1PlusStorage accepted_storage(factory);
    accepted_storage.stones[0].enabled = true;
    accepted_storage.stones[0].velocity_x = speed_limit.raw_value();
    accepted_storage.stones[0].velocity_y = 28377;
    EXPECT_NO_THROW(accepted_storage.CreateSimulator());

    dcs::SimulatorFCV1PlusStorage rejected_storage(factory);
    rejected_storage.stones[0].enabled = true;
    rejected_storage.stones[0].velocity_x = speed_limit.raw_value();
    rejected_storage.stones[0].velocity_y = 28378;
    EXPECT_THROW(rejected_storage.CreateSimulator(), std::overflow_error);
}

TEST(
    SimulatorFCV1Plus,
    ValidateRuntimeStateRejectsExtremeRawVelocityWithLengthOverflowMessage)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1PlusStorage storage(factory);
    storage.stones[0].enabled = true;
    storage.stones[0].velocity_x =
        std::numeric_limits<std::int32_t>::min();

    try {
        storage.CreateSimulator();
        FAIL() << "Expected std::overflow_error";
    }
    catch (std::overflow_error const& error) {
        EXPECT_STREQ(
            error.what(),
            "fcv1plus: vector length exceeds Q8.24 range.");
    }
}

TEST(
    SimulatorFCV1Plus,
    ValidateRuntimeStatePositionCheckTakesPrecedenceOverVelocityLengthOverflow)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1PlusStorage storage(factory);
    storage.stones[0].enabled = true;
    storage.stones[0].position_x =
        std::numeric_limits<std::int32_t>::min();
    storage.stones[0].velocity_x =
        std::numeric_limits<std::int32_t>::min();

    try {
        storage.CreateSimulator();
        FAIL() << "Expected std::overflow_error";
    }
    catch (std::overflow_error const& error) {
        EXPECT_STREQ(
            error.what(),
            "fcv1plus: state left the supported runtime range.");
    }
}

TEST(SimulatorFCV1Plus, SubEpsilonVelocityBecomesZeroAndUnblocksAreAllStonesStopped)
{
    // Regression for the Step()/AreAllStonesStopped() mismatch: Step() only
    // damped speeds strictly greater than epsilon, while
    // AreAllStonesStopped() treated any nonzero raw velocity as moving, so a
    // stone with 0 < speed <= epsilon was never touched by Step() yet never
    // reported as stopped either -- hanging any
    // `while (!AreAllStonesStopped()) Step();` caller.
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1PlusStorage storage(factory);
    storage.stones[0].enabled = true;
    storage.stones[0].velocity_x = 1;  // == Fixed::epsilon(), smallest nonzero raw value.
    auto simulator = dct::AsStepSimulator(storage.CreateSimulator());

    EXPECT_FALSE(simulator->AreAllStonesStopped());
    simulator->Step();
    EXPECT_TRUE(simulator->AreAllStonesStopped());

    auto const raw =
        dynamic_cast<dcs::SimulatorFCV1Plus&>(*simulator).GetRawStonesForTesting();
    EXPECT_EQ(raw[0].velocity_x, 0);
    EXPECT_EQ(raw[0].velocity_y, 0);
}

TEST(SimulatorFCV1Plus, CalculateShotRejectsRegressionDomain)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1Plus simulator(factory);
    EXPECT_THROW(
        simulator.CalculateShot(dc::Vector2(0.0f, 20.0f), 0.0f, 1.0f),
        std::invalid_argument);
    EXPECT_THROW(
        simulator.CalculateShot(dc::Vector2(0.0f, 8.0f), 2.0f, 1.0f),
        std::invalid_argument);
}

#ifdef DIGITALCURLING_TEST_HAVE_FCV1
TEST(SimulatorFCV1Plus, CalculateShotTracksFCV1AcrossBranchesAndSpinSigns)
{
    dcs::SimulatorFCV1Factory fcv1_factory;
    dcs::SimulatorFCV1 fcv1(fcv1_factory);
    dcs::SimulatorFCV1PlusFactory plus_factory;
    dcs::SimulatorFCV1Plus plus(plus_factory);
    dc::Vector2 const target(0.0f, 32.0f);

    for (float const speed : {0.02f, 0.8f, 2.0f}) {
        for (float const spin : {-1.0f, 1.0f}) {
            auto const reference = fcv1.CalculateShot(target, speed, spin);
            auto const actual = plus.CalculateShot(target, speed, spin);
            EXPECT_NEAR(
                reference.translational_velocity,
                actual.translational_velocity,
                kCalculateShotScalarTolerance);
            EXPECT_NEAR(
                reference.angular_velocity,
                actual.angular_velocity,
                kCalculateShotScalarTolerance);
            EXPECT_NEAR(
                reference.release_angle,
                actual.release_angle,
                kCalculateShotAngleTolerance);
        }
    }
}
#endif  // DIGITALCURLING_TEST_HAVE_FCV1

TEST(SimulatorFCV1Plus, CalculateShotForwardSimulationReachesTarget)
{
    dcs::SimulatorFCV1PlusFactory factory;
    dcs::SimulatorFCV1Plus simulator(factory);
    dc::Vector2 const target(0.0f, 32.0f);
    constexpr float target_speed = 0.8f;
    auto const shot = simulator.CalculateShot(target, target_speed, 1.0f);

    dcs::ISimulator::AllStones stones;
    stones[0].emplace(
        dc::Vector2(),
        0.0f,
        shot.ToVector2(),
        shot.angular_velocity);
    simulator.SetStones(stones);
    dc::Vector2 reached;
    while (!simulator.AreAllStonesStopped()) {
        auto const current = simulator.GetStones()[0].value();
        if (current.translational_velocity.Length() <= target_speed) {
            reached = current.position;
            break;
        }
        simulator.Step();
    }
    // dc::Vector2's member operator-(Vector2) (see vector2.hpp) is a binary
    // overload with the same signature as its own free-function operator-,
    // which is ambiguous for two non-const-qualified-friendly operands here.
    // Subtract component-wise instead of relying on operator- overload
    // resolution.
    dc::Vector2 const diff(reached.x - target.x, reached.y - target.y);
    EXPECT_LE(diff.Length(), kCalculateShotDistanceTolerance);
}

// Simulate() must give exactly what the old `SimulatorSimulate(Full, 0, width)` loop gave.
TEST(SimulatorFCV1Plus, SimulateMatchesManualStepLoop)
{
    dcs::SimulatorFCV1PlusFactory factory;
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

TEST(SimulatorFCV1Plus, StepUntilRestHonorsFrameLimit)
{
    dcs::SimulatorFCV1PlusFactory factory;
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
    dcs::SimulatorFCV1PlusFactory idle_factory;
    auto idle = dct::AsStepSimulator(idle_factory.CreateSimulator());
    EXPECT_EQ(dcs::StepUntilRest(*idle, 0, scenario.sheet_width), 0);
}

TEST(SimulatorFCV1Plus, SimulateMatchesRecordedBaseline)
{
    dcs::SimulatorFCV1PlusFactory factory;
    auto const& scenarios = dct::BaselineScenarios();
    ASSERT_EQ(scenarios.size(), dct::kFcv1PlusBaseline.size());
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        SCOPED_TRACE(scenarios[i].name);
        auto simulator = dct::AsStepSimulator(factory.CreateSimulator());
        simulator->SetStones(scenarios[i].initial);
        simulator->Simulate(scenarios[i].sheet_width);
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(simulator->GetStones()), dct::kFcv1PlusBaseline[i]));
    }
}

TEST(SimulatorFCV1Plus, GetStonesAndGetFactoryReturnCopies)
{
    dcs::SimulatorFCV1PlusFactory factory;
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
TEST(SimulatorFCV1Plus, LoadsLegacyStorageJsonWithCollisions)
{
    dcs::SimulatorFCV1PlusFactory factory;
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
        auto storage = legacy.get<dcs::SimulatorFCV1PlusStorage>();
        auto restored = dct::AsStepSimulator(storage.CreateSimulator());
        EXPECT_TRUE(dct::BitsEqual(dct::ToBaselineBits(restored->GetStones()), expected));
    }
}
