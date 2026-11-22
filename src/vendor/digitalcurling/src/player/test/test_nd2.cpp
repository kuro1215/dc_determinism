#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include "common.hpp"
#include "../src/nd2/player_nd2.hpp"
#include "../src/nd2/player_nd2_factory.hpp"

namespace dct = digitalcurling::test;

TEST(PlayerND2, SaveLoad1)
{
    dcp::PlayerND2Factory factory;
    dct::PlayerTestSaveLoad1(factory.CreatePlayer());
}

TEST(PlayerND2, SaveLoad2)
{
    dcp::PlayerND2Factory factory;
    dct::PlayerTestSaveLoad2(factory.CreatePlayer());
}

TEST(PlayerND2, SaveLoad3)
{
    dcp::PlayerND2Factory factory;
    dct::PlayerTestSaveLoad3(factory.CreatePlayer());
}

TEST(PlayerND2, SaveLoad4)
{
    dcp::PlayerND2Factory factory;
    dct::PlayerTestSaveLoad4<dcp::PlayerND2Storage>(factory.CreatePlayer());
}


TEST(PlayerND2, FactoryToJson)
{
    auto v_nd2 = std::make_unique<dcp::PlayerND2Factory>();
    v_nd2->gender = dcp::Gender::kMale;
    v_nd2->cw = dcp::PlayerND2Parameter { 7.0f, 0.1f, 0.2f, 0.3f };
    v_nd2->ccw = dcp::PlayerND2Parameter { 6.0f, 0.11f, 0.21f, 0.31f };
    v_nd2->seed = 3;

    json const j_nd2 = *v_nd2.get();
    EXPECT_EQ(j_nd2.at("type").get<std::string>(), "nd2");
    EXPECT_EQ(j_nd2.at("gender").get<dcp::Gender>(), v_nd2->gender);
    EXPECT_EQ(j_nd2.at("seed").get<std::random_device::result_type>(), v_nd2->seed.value());

    auto const& j_cw = j_nd2.at("cw");
    EXPECT_EQ(j_cw.at("max_speed").get<float>(), v_nd2->cw.max_speed);
    EXPECT_EQ(j_cw.at("stddev_speed").get<float>(), v_nd2->cw.stddev_speed);
    EXPECT_EQ(j_cw.at("stddev_angle").get<float>(), v_nd2->cw.stddev_angle);
    EXPECT_EQ(j_cw.at("stddev_rotation").get<float>(), v_nd2->cw.stddev_rotation);

    auto const& j_ccw = j_nd2.at("ccw");
    EXPECT_EQ(j_ccw.at("max_speed").get<float>(), v_nd2->ccw.max_speed);
    EXPECT_EQ(j_ccw.at("stddev_speed").get<float>(), v_nd2->ccw.stddev_speed);
    EXPECT_EQ(j_ccw.at("stddev_angle").get<float>(), v_nd2->ccw.stddev_angle);
    EXPECT_EQ(j_ccw.at("stddev_rotation").get<float>(), v_nd2->ccw.stddev_rotation);
}

TEST(PlayerND2, FactoryFromJson)
{
    json const j_nd2{
        { "type", "nd2" },
        { "gender", "female" },
        { "cw", {
            { "max_speed", 7.f },
            { "stddev_speed", 0.11f },
            { "stddev_angle", 0.21f },
            { "stddev_rotation", 0.31f }
        }},
        { "ccw", {
            { "max_speed", 6.f },
            { "stddev_speed", 0.12f },
            { "stddev_angle", 0.22f },
            { "stddev_rotation", 0.32f }
        }},
        { "seed", 5 }
    };

    dcp::PlayerND2Factory v_nd2;
    EXPECT_NO_THROW(v_nd2 = j_nd2.get<dcp::PlayerND2Factory>());
    EXPECT_EQ(j_nd2.at("gender").get<dcp::Gender>(), v_nd2.gender);
    EXPECT_EQ(j_nd2.at("seed").get<std::random_device::result_type>(), v_nd2.seed.value());

    auto const& j_cw = j_nd2.at("cw");
    EXPECT_EQ(j_cw.at("max_speed").get<float>(), v_nd2.cw.max_speed);
    EXPECT_EQ(j_cw.at("stddev_speed").get<float>(), v_nd2.cw.stddev_speed);
    EXPECT_EQ(j_cw.at("stddev_angle").get<float>(), v_nd2.cw.stddev_angle);
    EXPECT_EQ(j_cw.at("stddev_rotation").get<float>(), v_nd2.cw.stddev_rotation);

    auto const& j_ccw = j_nd2.at("ccw");
    EXPECT_EQ(j_ccw.at("max_speed").get<float>(), v_nd2.ccw.max_speed);
    EXPECT_EQ(j_ccw.at("stddev_speed").get<float>(), v_nd2.ccw.stddev_speed);
    EXPECT_EQ(j_ccw.at("stddev_angle").get<float>(), v_nd2.ccw.stddev_angle);
    EXPECT_EQ(j_ccw.at("stddev_rotation").get<float>(), v_nd2.ccw.stddev_rotation);
}

TEST(PlayerND2, GetFactoryCarriesTheEffectiveSeed)
{
    dcp::PlayerND2Factory factory;  // no seed: the player picks one
    ASSERT_FALSE(factory.seed.has_value());
    auto player = factory.CreatePlayer();

    auto first = player->GetFactory();
    auto second = player->GetFactory();
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), second.get());
    auto* const first_factory = dynamic_cast<dcp::PlayerND2Factory*>(first.get());
    ASSERT_NE(first_factory, nullptr);
    EXPECT_TRUE(first_factory->seed.has_value());

    // A player created from the copy starts from the same state as the original.
    auto const shot = dc::moves::Shot(2.f, dct::kHalfPi, 0.1f);
    auto clone = first->CreatePlayer();
    EXPECT_TRUE(dct::EqualsShot(player->Play(shot), clone->Play(shot)));
}

TEST(PlayerND2, FactoryFromJsonValidatesParameters)
{
    auto make = [](dcp::PlayerND2Parameter cw, dcp::PlayerND2Parameter ccw) {
        json j{
            { "type", "nd2" },
            { "gender", "female" },
            { "cw", cw },
            { "ccw", ccw }
        };
        return j;
    };
    dcp::PlayerND2Parameter const ok { 4.f, 0.1f, 0.2f, 0.3f };
    float const inf = std::numeric_limits<float>::infinity();

    EXPECT_NO_THROW(make(ok, ok).get<dcp::PlayerND2Factory>());

    // stddev 0 means no noise and is allowed
    EXPECT_NO_THROW(make({ 4.f, 0.f, 0.f, 0.f }, ok).get<dcp::PlayerND2Factory>());

    EXPECT_THROW(make({ 0.f, 0.1f, 0.2f, 0.3f }, ok).get<dcp::PlayerND2Factory>(), std::invalid_argument);
    EXPECT_THROW(make(ok, { -1.f, 0.1f, 0.2f, 0.3f }).get<dcp::PlayerND2Factory>(), std::invalid_argument);
    EXPECT_THROW(make({ 4.f, -0.1f, 0.2f, 0.3f }, ok).get<dcp::PlayerND2Factory>(), std::invalid_argument);
    EXPECT_THROW(make(ok, { 4.f, 0.1f, -0.2f, 0.3f }).get<dcp::PlayerND2Factory>(), std::invalid_argument);
    EXPECT_THROW(make({ 4.f, 0.1f, 0.2f, -0.3f }, ok).get<dcp::PlayerND2Factory>(), std::invalid_argument);

    // Infinity and NaN are serialized as null by JSON
    EXPECT_ANY_THROW(make({ inf, 0.1f, 0.2f, 0.3f }, ok).get<dcp::PlayerND2Factory>());
    EXPECT_ANY_THROW(make(ok, { 4.f, std::numeric_limits<float>::quiet_NaN(), 0.2f, 0.3f }).get<dcp::PlayerND2Factory>());
}

TEST(PlayerND2, CreatePlayerRejectsInvalidFactory)
{
    float const nan = std::numeric_limits<float>::quiet_NaN();
    float const inf = std::numeric_limits<float>::infinity();

    dcp::PlayerND2Factory factory;
    EXPECT_NO_THROW(factory.CreatePlayer());

    factory = {};
    factory.cw.max_speed = nan;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.ccw.max_speed = 0.f;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.cw.stddev_speed = inf;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.ccw.stddev_angle = -0.1f;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
    factory = {};
    factory.cw.stddev_rotation = nan;
    EXPECT_THROW(factory.CreatePlayer(), std::invalid_argument);
}

TEST(PlayerND2, StorageRejectsInvalidEngineData)
{
    dcp::PlayerND2Factory factory;
    factory.seed = 7;
    auto player = factory.CreatePlayer();
    auto storage = dynamic_cast<dcp::PlayerND2Storage&>(*player->CreateStorage());

    EXPECT_NO_THROW(storage.CreatePlayer());

    auto const expect_rejected = [&](std::string const& engine_data) {
        auto bad = storage;
        bad.engine_data = engine_data;
        EXPECT_THROW(bad.CreatePlayer(), std::invalid_argument) << engine_data;
        EXPECT_THROW(player->Load(bad), std::invalid_argument) << engine_data;
    };

    expect_rejected("");
    expect_rejected("not a state");
    // Only three of the four words
    expect_rejected("0123456789abcdef 0123456789abcdef 0123456789abcdef");
    // Trailing garbage
    expect_rejected(storage.engine_data + " 1");
    expect_rejected(storage.engine_data + " x");
    // All-zero state
    expect_rejected("0000000000000000 0000000000000000 0000000000000000 0000000000000000");
}

TEST(PlayerND2, FailedLoadKeepsPlayerState)
{
    dcp::PlayerND2Factory factory;
    factory.seed = 11;
    factory.cw.stddev_speed = 0.1f;
    factory.cw.stddev_angle = 0.1f;
    factory.cw.stddev_rotation = 0.1f;
    auto player = factory.CreatePlayer();
    auto reference = factory.CreatePlayer();
    auto shot = dc::moves::Shot(0.1f, -dct::kHalfPi, 2.f);

    auto storage = dynamic_cast<dcp::PlayerND2Storage&>(*player->CreateStorage());
    storage.engine_data = "0123456789abcdef 0123456789abcdef";
    EXPECT_THROW(player->Load(storage), std::invalid_argument);

    EXPECT_TRUE(dct::EqualsShot(player->Play(shot), reference->Play(shot)));
}

TEST(PlayerND2, StoredStateReproducesShots)
{
    dcp::PlayerND2Factory factory;
    factory.seed = 21;
    factory.cw.stddev_speed = 0.1f;
    factory.cw.stddev_angle = 0.1f;
    factory.cw.stddev_rotation = 0.1f;
    auto player = factory.CreatePlayer();
    auto shot = dc::moves::Shot(0.1f, -dct::kHalfPi, 2.f);
    player->Play(shot);

    json const j = *std::dynamic_pointer_cast<dcp::PlayerND2Storage>(
        std::shared_ptr<dcp::IPlayerStorage>(player->CreateStorage()));
    auto restored = j.get<dcp::PlayerND2Storage>().CreatePlayer();

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(dct::EqualsShot(player->Play(shot), restored->Play(shot)));
    }
}
