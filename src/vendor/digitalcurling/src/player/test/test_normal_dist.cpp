#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include "common.hpp"
#include "../src/normal_dist/player_normal_dist.hpp"
#include "../src/normal_dist/player_normal_dist_factory.hpp"

namespace dct = digitalcurling::test;

TEST(PlayerNormalDist, SaveLoad1)
{
    dcp::PlayerNormalDistFactory factory;
    dct::PlayerTestSaveLoad1(factory.CreatePlayer());
}

TEST(PlayerNormalDist, SaveLoad2)
{
    dcp::PlayerNormalDistFactory factory;
    dct::PlayerTestSaveLoad2(factory.CreatePlayer());
}

TEST(PlayerNormalDist, SaveLoad3)
{
    dcp::PlayerNormalDistFactory factory;
    dct::PlayerTestSaveLoad3(factory.CreatePlayer());
}

TEST(PlayerNormalDist, SaveLoad4)
{
    dcp::PlayerNormalDistFactory factory;
    dct::PlayerTestSaveLoad4<dcp::PlayerNormalDistStorage>(factory.CreatePlayer());
}


TEST(PlayerNormalDist, FactoryToJson)
{
    auto v_normal_dist = std::make_unique<dc::players::PlayerNormalDistFactory>();
    v_normal_dist->gender = dcp::Gender::kMale;
    v_normal_dist->max_speed = 5.0f;
    v_normal_dist->stddev_speed = 0.1f;
    v_normal_dist->stddev_angle = 0.2f;
    v_normal_dist->seed = 3;

    json const j_normal_dist = *v_normal_dist.get();
    EXPECT_EQ(j_normal_dist.at("type").get<std::string>(), "normal_dist");
    EXPECT_EQ(j_normal_dist.at("gender").get<dcp::Gender>(), v_normal_dist->gender);
    EXPECT_EQ(j_normal_dist.at("max_speed").get<float>(), v_normal_dist->max_speed);
    EXPECT_EQ(j_normal_dist.at("stddev_speed").get<float>(), v_normal_dist->stddev_speed);
    EXPECT_EQ(j_normal_dist.at("stddev_angle").get<float>(), v_normal_dist->stddev_angle);
    EXPECT_EQ(j_normal_dist.at("seed").get<std::random_device::result_type>(), v_normal_dist->seed.value());
}

TEST(PlayerNormalDist, FactoryFromJsonWithoutSeed)
{
    json const j_normal_dist{
        { "type", "normal_dist" },
        { "gender", "female" },
        { "max_speed", 8.f },
        { "stddev_speed", 0.1f },
        { "stddev_angle", 0.2f }
    };

    dcp::PlayerNormalDistFactory v_normal_dist;
    v_normal_dist.seed = 4;
    EXPECT_NO_THROW(j_normal_dist.get_to(v_normal_dist));
    EXPECT_FALSE(v_normal_dist.seed.has_value());
}

TEST(PlayerNormalDist, FactoryFromJson)
{

    json const j_normal_dist{
        { "type", "normal_dist" },
        { "gender", "female" },
        { "max_speed", 8.f },
        { "stddev_speed", 0.1f },
        { "stddev_angle", 0.2f },
        { "seed", 4 }
    };

    dcp::PlayerNormalDistFactory v_normal_dist;
    EXPECT_NO_THROW(v_normal_dist = j_normal_dist.get<dcp::PlayerNormalDistFactory>());

    EXPECT_EQ(j_normal_dist.at("gender").get<dcp::Gender>(), v_normal_dist.gender);
    EXPECT_EQ(j_normal_dist.at("max_speed").get<float>(), v_normal_dist.max_speed);
    EXPECT_EQ(j_normal_dist.at("stddev_speed").get<float>(), v_normal_dist.stddev_speed);
    EXPECT_EQ(j_normal_dist.at("stddev_angle").get<float>(), v_normal_dist.stddev_angle);
    EXPECT_EQ(j_normal_dist.at("seed").get<std::random_device::result_type>(), v_normal_dist.seed.value());
}

TEST(PlayerNormalDist, GetFactoryCarriesTheEffectiveSeed)
{
    dcp::PlayerNormalDistFactory factory;  // no seed: the player picks one
    ASSERT_FALSE(factory.seed.has_value());
    auto player = factory.CreatePlayer();

    auto first = player->GetFactory();
    auto second = player->GetFactory();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get());
    auto* const first_factory = dynamic_cast<dcp::PlayerNormalDistFactory*>(first.get());
    ASSERT_NE(first_factory, nullptr);
    EXPECT_TRUE(first_factory->seed.has_value());

    // A player created from the copy starts from the same state as the original.
    auto const shot = dc::moves::Shot(2.f, dct::kHalfPi, 0.1f);
    auto clone = first->CreatePlayer();
    EXPECT_TRUE(dct::EqualsShot(player->Play(shot), clone->Play(shot)));
}

TEST(PlayerNormalDist, FactoryFromJsonRejectsInvalidParameters)
{
    auto make = [](float max_speed, float stddev_speed, float stddev_angle) {
        return json{
            { "type", "normal_dist" },
            { "gender", "female" },
            { "max_speed", max_speed },
            { "stddev_speed", stddev_speed },
            { "stddev_angle", stddev_angle }
        };
    };
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();

    EXPECT_NO_THROW(make(4.f, 0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>());
    EXPECT_THROW(make(0.f, 0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);
    EXPECT_THROW(make(-1.f, 0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);
    EXPECT_THROW(make(4.f, 0.f, 0.2f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);
    EXPECT_THROW(make(4.f, -0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);
    EXPECT_THROW(make(4.f, 0.1f, 0.f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);
    EXPECT_THROW(make(4.f, 0.1f, -0.2f).get<dcp::PlayerNormalDistFactory>(), std::invalid_argument);

    // NaN and infinity are not representable in JSON numbers (serialized as null).
    EXPECT_ANY_THROW(make(nan, 0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>());
    EXPECT_ANY_THROW(make(inf, 0.1f, 0.2f).get<dcp::PlayerNormalDistFactory>());
    EXPECT_ANY_THROW(make(4.f, nan, 0.2f).get<dcp::PlayerNormalDistFactory>());
    EXPECT_ANY_THROW(make(4.f, 0.1f, inf).get<dcp::PlayerNormalDistFactory>());
}

TEST(PlayerNormalDist, CreatePlayerRejectsInvalidFactory)
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();

    dcp::PlayerNormalDistFactory factory;
    EXPECT_NO_THROW(factory.CreatePlayer());

    factory = {};
    factory.max_speed = nan;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.max_speed = 0.f;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.stddev_speed = inf;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.stddev_speed = 0.f;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.stddev_angle = nan;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.stddev_angle = -1.f;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
}

TEST(PlayerNormalDist, StorageRejectsInvalidState)
{
    dcp::PlayerNormalDistFactory factory;
    factory.seed = 7;
    auto player = factory.CreatePlayer();
    auto storage = dynamic_cast<dcp::PlayerNormalDistStorage&>(*player->CreateStorage());

    EXPECT_NO_THROW(storage.CreatePlayer());

    // Truncated engine data
    {
        auto bad = storage;
        bad.engine_data = bad.engine_data.substr(0, bad.engine_data.size() / 2);
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    // Trailing garbage
    {
        auto bad = storage;
        bad.engine_data += " x";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    // Empty and non-numeric data
    {
        auto bad = storage;
        bad.engine_data = "";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
        bad.engine_data = "not a state";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    {
        auto bad = storage;
        bad.speed_dist_data = "";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    {
        auto bad = storage;
        bad.angle_dist_data += " 1";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    // Distribution with a zero standard deviation
    {
        auto bad = storage;
        bad.speed_dist_data = "0 0 0 0";
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
    // Invalid factory in storage
    {
        auto bad = storage;
        bad.factory.stddev_speed = 0.f;
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument);
    }
}

TEST(PlayerNormalDist, StorageFromJsonRequiresEngineData)
{
    dcp::PlayerNormalDistFactory factory;
    factory.seed = 7;
    auto player = factory.CreatePlayer();
    json j = *std::dynamic_pointer_cast<dcp::PlayerNormalDistStorage>(
        std::shared_ptr<dcp::IPlayerStorage>(player->CreateStorage()));

    EXPECT_NO_THROW(j.get<dcp::PlayerNormalDistStorage>());

    j["engine_data"] = "";
    EXPECT_THROW(j.get<dcp::PlayerNormalDistStorage>(), std::invalid_argument);
}

TEST(PlayerNormalDist, FailedLoadKeepsPlayerState)
{
    dcp::PlayerNormalDistFactory factory;
    factory.seed = 11;
    auto player = factory.CreatePlayer();
    auto reference = factory.CreatePlayer();
    auto shot = dc::moves::Shot(0.1f, -dct::kHalfPi, 2.f);

    auto storage = dynamic_cast<dcp::PlayerNormalDistStorage&>(*player->CreateStorage());
    storage.engine_data = storage.engine_data.substr(0, storage.engine_data.size() / 2);
    EXPECT_THROW(player->Load(storage), std::invalid_argument);

    EXPECT_TRUE(dct::EqualsShot(player->Play(shot), reference->Play(shot)));
}

TEST(PlayerNormalDist, StoredStateReproducesShots)
{
    dcp::PlayerNormalDistFactory factory;
    factory.seed = 21;
    auto player = factory.CreatePlayer();
    auto shot = dc::moves::Shot(0.1f, -dct::kHalfPi, 2.f);
    player->Play(shot);

    json const j = *std::dynamic_pointer_cast<dcp::PlayerNormalDistStorage>(
        std::shared_ptr<dcp::IPlayerStorage>(player->CreateStorage()));
    auto restored = j.get<dcp::PlayerNormalDistStorage>().CreatePlayer();

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(dct::EqualsShot(player->Play(shot), restored->Play(shot)));
    }
}
