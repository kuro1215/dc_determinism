#include <memory>
#include <string>
#include "common.hpp"
#include "../src/identical/player_identical.hpp"
#include "../src/identical/player_identical_factory.hpp"

namespace dct = digitalcurling::test;

TEST(PlayerIdentical, SaveLoad1)
{
    dcp::PlayerIdenticalFactory factory;
    dct::PlayerTestSaveLoad1(factory.CreatePlayer());
}

TEST(PlayerIdentical, SaveLoad2)
{
    dcp::PlayerIdenticalFactory factory;
    dct::PlayerTestSaveLoad2(factory.CreatePlayer());
}

TEST(PlayerIdentical, SaveLoad3)
{
    dcp::PlayerIdenticalFactory factory;
    dct::PlayerTestSaveLoad3(factory.CreatePlayer());
}

TEST(PlayerIdentical, SaveLoad4)
{
    dcp::PlayerIdenticalFactory factory;
    dct::PlayerTestSaveLoad4<dcp::PlayerIdenticalStorage>(factory.CreatePlayer());
}


TEST(PlayerIdentical, FactoryToJson)
{
    auto v_identical = std::make_unique<dcp::PlayerIdenticalFactory>();
    v_identical->gender = dcp::Gender::kMale;

    json const j_identical = *v_identical.get();
    EXPECT_EQ(j_identical.at("type").get<std::string>(), "identical");
    EXPECT_EQ(j_identical.at("gender").get<dcp::Gender>(), v_identical->gender);
}

TEST(PlayerIdentical, FactoryFromJson)
{
    json const j_identical{
        { "type", "identical" },
        { "gender", "female" }
    };

    dcp::PlayerIdenticalFactory v_identical;
    EXPECT_NO_THROW(v_identical = j_identical.get<dcp::PlayerIdenticalFactory>());
    EXPECT_EQ(j_identical.at("gender").get<dcp::Gender>(), v_identical.gender);
}

TEST(PlayerIdentical, GetFactoryReturnsIndependentCopies)
{
    dcp::PlayerIdenticalFactory male_factory;
    male_factory.gender = dcp::Gender::kMale;
    dcp::PlayerIdenticalFactory female_factory;
    female_factory.gender = dcp::Gender::kFemale;
    auto male = male_factory.CreatePlayer();
    auto female = female_factory.CreatePlayer();

    auto male_copy = male->GetFactory();
    auto female_copy = female->GetFactory();
    ASSERT_NE(male_copy, nullptr);
    ASSERT_NE(female_copy, nullptr);
    EXPECT_NE(male_copy.get(), female_copy.get());
    EXPECT_EQ(male_copy->GetGender(), dcp::Gender::kMale);
    EXPECT_EQ(female_copy->GetGender(), dcp::Gender::kFemale);

    // A copy is not affected by later changes of the player.
    auto const storage = female->CreateStorage();
    male->Load(*storage);
    EXPECT_EQ(male_copy->GetGender(), dcp::Gender::kMale);
    EXPECT_EQ(male->GetFactory()->GetGender(), dcp::Gender::kFemale);
}
