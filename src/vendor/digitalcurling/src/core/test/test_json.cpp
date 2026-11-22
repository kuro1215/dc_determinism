// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "digitalcurling/core.hpp"
#include "digitalcurling/players/gender.hpp"

namespace dc = digitalcurling;
using nlohmann::json;

TEST(Json, millisecondsToJson)
{
    std::chrono::milliseconds const v(56'789);
    json const j = v;
    EXPECT_EQ(j.get<std::chrono::milliseconds>(), v);
}

TEST(Json, millisecondsFromJson)
{
    json const j = 1.234;
    std::chrono::milliseconds v;
    j.get_to(v);
    EXPECT_EQ(v.count(), 1234);
}

TEST(Json, Vector2ToJson)
{
    dc::Vector2 const v(1.5f, 2.5f);
    json const j = v;
    EXPECT_EQ(j.at("x").get<float>(), v.x);
    EXPECT_EQ(j.at("y").get<float>(), v.y);
}

TEST(Json, Vector2FromJson)
{
    json const j{
        { "x", 3.5f },
        { "y", 5.5f }
    };
    dc::Vector2 v;
    j.get_to(v);
    EXPECT_EQ(j.at("x").get<float>(), v.x);
    EXPECT_EQ(j.at("y").get<float>(), v.y);
}

TEST(Json, StoneToJson)
{
    dc::Stone const v(dc::Vector2(0.5f, 1.5f), 2.4f);
    json const j = v;
    EXPECT_EQ(j.at("position").at("x").get<float>(), v.position.x);
    EXPECT_EQ(j.at("position").at("y").get<float>(), v.position.y);
    EXPECT_EQ(j.at("angle").get<float>(), v.angle);
}

TEST(Json, StoneFromJson)
{
    json const j{
        { "position", {
            { "x", 1.5f },
            { "y", 2.5f }
        }},
        { "angle", 3.5f }
    };
    dc::Stone v;
    j.get_to(v);
    EXPECT_EQ(j.at("position").at("x").get<float>(), v.position.x);
    EXPECT_EQ(j.at("position").at("y").get<float>(), v.position.y);
    EXPECT_EQ(j.at("angle").get<float>(), v.angle);
}

TEST(Json, MoveToJson)
{
    // shot
    dc::moves::Shot v_shot(1.2f, 1.0f, 1.5f);
    dc::moves::Move v_shot_move = v_shot;
    json const j_shot = v_shot_move;
    EXPECT_EQ(j_shot.at("type").get<std::string>(), "shot");
    EXPECT_EQ(j_shot.at("translational_velocity").get<float>(), v_shot.translational_velocity);
    EXPECT_EQ(j_shot.at("angular_velocity").get<float>(), v_shot.angular_velocity);
    EXPECT_EQ(j_shot.at("release_angle").get<float>(), v_shot.release_angle);

    // concede
    dc::moves::Move const v_concede = dc::moves::Concede();
    json const j_concede = v_concede;
    EXPECT_EQ(j_concede.at("type"), "concede");
}

TEST(Json, MoveFromJson)
{
    // shot
    json const j_shot{
        { "type", "shot" },
        { "translational_velocity", 1.2f },
        { "angular_velocity", 1.0f },
        { "release_angle", 1.5f }
    };
    dc::moves::Move v_shot;
    j_shot.get_to(v_shot);
    if (std::holds_alternative<dc::moves::Shot>(v_shot)) {
        auto const& v_shot2 = std::get<dc::moves::Shot>(v_shot);
        EXPECT_EQ(j_shot.at("translational_velocity").get<float>(), v_shot2.translational_velocity);
        EXPECT_EQ(j_shot.at("angular_velocity").get<float>(), v_shot2.angular_velocity);
        EXPECT_EQ(j_shot.at("release_angle").get<float>(), v_shot2.release_angle);
    } else {
        FAIL();
    }

    // concede
    json const j_concede{
        { "type", "concede" }
    };
    dc::moves::Move v_concede;
    j_concede.get_to(v_concede);
    EXPECT_TRUE(std::holds_alternative<dc::moves::Concede>(v_concede));
}

TEST(Json, TeamToJson)
{
    json const j_team0 = dc::Team::k0;
    json const j_team1 = dc::Team::k1;
    EXPECT_EQ(j_team0.get<std::string>(), "team0");
    EXPECT_EQ(j_team1.get<std::string>(), "team1");
}

TEST(Json, TeamFromJson)
{
    json const j_team0 = "team0";
    json const j_team1 = "team1";
    dc::Team v_team0, v_team1;
    j_team0.get_to(v_team0);
    j_team1.get_to(v_team1);
    EXPECT_EQ(v_team0, dc::Team::k0);
    EXPECT_EQ(v_team1, dc::Team::k1);
}

TEST(Json, GameRuleTypeToJson)
{
    json const j_standard = dc::GameRuleType::kStandard;
    json const j_mixed = dc::GameRuleType::kMixed;
    json const j_mixed_doubles = dc::GameRuleType::kMixedDoubles;
    EXPECT_EQ(j_standard.get<std::string>(), "standard");
    EXPECT_EQ(j_mixed.get<std::string>(), "mixed");
    EXPECT_EQ(j_mixed_doubles.get<std::string>(), "mixed_doubles");
}

TEST(Json, GameRuleTypeFromJson)
{
    json const j_standard = "standard";
    json const j_mixed = "mixed";
    json const j_mixed_doubles = "mixed_doubles";
    dc::GameRuleType v_standard, v_mixed, v_mixed_doubles;
    j_standard.get_to(v_standard);
    j_mixed.get_to(v_mixed);
    j_mixed_doubles.get_to(v_mixed_doubles);
    EXPECT_EQ(v_standard, dc::GameRuleType::kStandard);
    EXPECT_EQ(v_mixed, dc::GameRuleType::kMixed);
    EXPECT_EQ(v_mixed_doubles, dc::GameRuleType::kMixedDoubles);
}

TEST(Json, PositionedStonesPatternJsonRoundTrip)
{
    struct PatternTestCase {
        dc::rules::PositionedStonesPattern pattern;
        int id;
        std::string token;
    };

    const std::array<PatternTestCase, 8> test_cases = {{
        { dc::rules::PositionedStonesPattern::kPosition2Back, 0, "position2_back" },
        { dc::rules::PositionedStonesPattern::kPosition2Front, 1, "position2_front" },
        { dc::rules::PositionedStonesPattern::kPosition1Back, 2, "position1_back" },
        { dc::rules::PositionedStonesPattern::kPosition1Front, 3, "position1_front" },
        { dc::rules::PositionedStonesPattern::kPosition3Back, 4, "position3_back" },
        { dc::rules::PositionedStonesPattern::kPosition3Front, 5, "position3_front" },
        { dc::rules::PositionedStonesPattern::kPosition4Back, 6, "position4_back" },
        { dc::rules::PositionedStonesPattern::kPosition4Front, 7, "position4_front" },
    }};

    for (const auto& tc : test_cases) {
        EXPECT_EQ(static_cast<int>(tc.pattern), tc.id);
        EXPECT_EQ(static_cast<dc::rules::PositionedStonesPattern>(tc.id), tc.pattern);

        json const j = tc.pattern;
        EXPECT_EQ(j.get<std::string>(), tc.token);
        EXPECT_EQ(j.get<dc::rules::PositionedStonesPattern>(), tc.pattern);

        json const j_from_token = tc.token;
        EXPECT_EQ(j_from_token.get<dc::rules::PositionedStonesPattern>(), tc.pattern);
    }

}

TEST(Json, AdditionalRuleTypesToJson)
{
    json const j_fgz = dc::rules::AdditionalRuleTypes::kFreeGuardZone;
    json const j_nts = dc::rules::AdditionalRuleTypes::kNoTickShot;
    EXPECT_EQ(j_fgz, "free_guard_zone");
    EXPECT_EQ(j_nts, "no_tick_shot");
}

TEST(Json, AdditionalRuleTypesFromJson)
{
    json const j_fgz = "free_guard_zone";
    json const j_nts = "no_tick_shot";
    dc::rules::AdditionalRuleTypes v_fgz;
    dc::rules::AdditionalRuleTypes v_nts;
    j_fgz.get_to(v_fgz);
    j_nts.get_to(v_nts);
    EXPECT_EQ(v_fgz, dc::rules::AdditionalRuleTypes::kFreeGuardZone);
    EXPECT_EQ(v_nts, dc::rules::AdditionalRuleTypes::kNoTickShot);
}

TEST(Json, AdditionalRuleToJson)
{
    dc::rules::FreeGuardZoneRule v_fgz;
    v_fgz.applied_shot_count = 5;
    json const j = v_fgz;
    EXPECT_EQ(j.at("applied_shot_count").get<std::uint8_t>(), v_fgz.applied_shot_count);

    dc::rules::NoTickShotRule v_nts;
    v_nts.applied_shot_count = 5;
    json const j2 = v_nts;
    EXPECT_EQ(j2.at("applied_shot_count").get<std::uint8_t>(), v_nts.applied_shot_count);
}

TEST(Json, AdditionalRuleFromJson)
{
    json const j_fgz{
        { "applied_shot_count", 5 }
    };
    dc::rules::FreeGuardZoneRule v_fgz;
    j_fgz.get_to(v_fgz);
    EXPECT_EQ(v_fgz.applied_shot_count, 5);

    json const j_nts{
        { "applied_shot_count", 5 }
    };
    dc::rules::NoTickShotRule v_nts;
    j_nts.get_to(v_nts);
    EXPECT_EQ(v_nts.applied_shot_count, 5);
}

TEST(Json, GameRuleToJson)
{
    dc::GameRule v;
    v.type = dc::GameRuleType::kStandard;
    v.regular_end_count = 10;
    v.is_wheelchair = false;
    v.positioned_stones = std::nullopt;
    v.free_guard_zone = dc::rules::FreeGuardZoneRule();
    v.no_tick_shot = dc::rules::NoTickShotRule();

    json const j = v;
    EXPECT_EQ(j.at("type").get<std::string>(), "standard");
    EXPECT_EQ(j.at("regular_end_count").get<std::uint8_t>(), v.regular_end_count);
    EXPECT_EQ(j.at("is_wheelchair").get<bool>(), v.is_wheelchair);
    EXPECT_TRUE(j.at("positioned_stones").is_null());
    EXPECT_EQ(j.at("free_guard_zone").at("applied_shot_count").get<std::uint8_t>(), v.free_guard_zone->applied_shot_count);
    EXPECT_EQ(j.at("no_tick_shot").at("applied_shot_count").get<std::uint8_t>(), v.no_tick_shot->applied_shot_count);
}

TEST(Json, GameRuleFromJson)
{
    json const j{
        { "type", "mixed_doubles" },
        { "regular_end_count", 8 },
        { "is_wheelchair", false },
        { "positioned_stones", "position1_front" }
    };

    dc::GameRule v;
    j.get_to(v);
    EXPECT_EQ(v.type, dc::GameRuleType::kMixedDoubles);
    EXPECT_EQ(v.regular_end_count, 8);
    EXPECT_FALSE(v.is_wheelchair);
    ASSERT_TRUE(v.positioned_stones.has_value());
    EXPECT_EQ(v.positioned_stones.value(), dc::rules::PositionedStonesPattern::kPosition1Front);
    EXPECT_FALSE(v.free_guard_zone.has_value());
    EXPECT_FALSE(v.no_tick_shot.has_value());
}

TEST(GameRule, MixedDoublesRequiresPositionedStones)
{
    EXPECT_THROW(
        dc::GameRule(dc::GameRuleType::kMixedDoubles, false, 8, true, true),
        std::invalid_argument
    );

    dc::GameRule const rule(
        dc::GameRuleType::kMixedDoubles,
        false,
        8,
        true,
        true,
        dc::rules::PositionedStonesPattern::kPosition1Front
    );

    EXPECT_EQ(rule.regular_end_count, 8);
    ASSERT_TRUE(rule.free_guard_zone.has_value());
    ASSERT_TRUE(rule.no_tick_shot.has_value());
    EXPECT_EQ(rule.free_guard_zone->applied_shot_count, 3);
    EXPECT_EQ(rule.no_tick_shot->applied_shot_count, 3);
}

TEST(GameRule, ShotRuleDefaultsFollowGameRuleType)
{
    dc::rules::FreeGuardZoneRule const standard_fgz(dc::GameRuleType::kStandard);
    dc::rules::FreeGuardZoneRule const mixed_doubles_fgz(dc::GameRuleType::kMixedDoubles);
    dc::rules::NoTickShotRule const standard_no_tick(dc::GameRuleType::kStandard);
    dc::rules::NoTickShotRule const mixed_doubles_no_tick(dc::GameRuleType::kMixedDoubles);

    EXPECT_EQ(standard_fgz.applied_shot_count, 5);
    EXPECT_EQ(mixed_doubles_fgz.applied_shot_count, 3);
    EXPECT_EQ(standard_no_tick.applied_shot_count, 5);
    EXPECT_EQ(mixed_doubles_no_tick.applied_shot_count, 3);
}

TEST(PositionedStones, PowerPlayPositionsUseExpectedSide)
{
    auto const hammer_left = dc::coordinate::GetHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesType::kPowerPlayLeft
    );
    auto const hammer_right = dc::coordinate::GetHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesType::kPowerPlayRight
    );
    auto const non_hammer_left = dc::coordinate::GetNonHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesPattern::kPosition1Front,
        dc::rules::PositionedStonesType::kPowerPlayLeft
    );
    auto const non_hammer_right = dc::coordinate::GetNonHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesPattern::kPosition1Front,
        dc::rules::PositionedStonesType::kPowerPlayRight
    );

    EXPECT_LT(hammer_left.x, 0.f);
    EXPECT_GT(hammer_right.x, 0.f);
    EXPECT_FLOAT_EQ(hammer_left.y, hammer_right.y);
    EXPECT_LT(non_hammer_left.x, 0.f);
    EXPECT_GT(non_hammer_right.x, 0.f);
    EXPECT_FLOAT_EQ(non_hammer_left.y, non_hammer_right.y);
}

TEST(PositionedStones, PositionA2026Coordinates)
{
    struct PatternExpected {
        dc::rules::PositionedStonesPattern pattern;
        float expected_y;
        float expected_pp_x;
    };

    const std::array<PatternExpected, 8> patterns = {{
        { dc::rules::PositionedStonesPattern::kPosition1Front, 35.059f, 1.12f },
        { dc::rules::PositionedStonesPattern::kPosition1Back,  35.349f, 1.12f },
        { dc::rules::PositionedStonesPattern::kPosition2Front, 34.450f, 1.10f },
        { dc::rules::PositionedStonesPattern::kPosition2Back,  34.740f, 1.10f },
        { dc::rules::PositionedStonesPattern::kPosition3Front, 33.840f, 1.08f },
        { dc::rules::PositionedStonesPattern::kPosition3Back,  34.130f, 1.08f },
        { dc::rules::PositionedStonesPattern::kPosition4Front, 33.231f, 1.07f },
        { dc::rules::PositionedStonesPattern::kPosition4Back,  33.521f, 1.07f },
    }};

    for (const auto& item : patterns) {
        // Center Guard: X = 0, Y = official absolute value
        auto const pt_center = dc::coordinate::GetNonHammerTeamPositionedStonePoint(
            item.pattern, dc::rules::PositionedStonesType::kCenterGuard
        );
        EXPECT_FLOAT_EQ(pt_center.x, 0.f);
        EXPECT_FLOAT_EQ(pt_center.y, item.expected_y);

        // Power Play Left: X = -expected_pp_x, Y = official absolute value
        auto const pt_left = dc::coordinate::GetNonHammerTeamPositionedStonePoint(
            item.pattern, dc::rules::PositionedStonesType::kPowerPlayLeft
        );
        EXPECT_FLOAT_EQ(pt_left.x, -item.expected_pp_x);
        EXPECT_FLOAT_EQ(pt_left.y, item.expected_y);

        // Power Play Right: X = +expected_pp_x, Y = official absolute value
        auto const pt_right = dc::coordinate::GetNonHammerTeamPositionedStonePoint(
            item.pattern, dc::rules::PositionedStonesType::kPowerPlayRight
        );
        EXPECT_FLOAT_EQ(pt_right.x, item.expected_pp_x);
        EXPECT_FLOAT_EQ(pt_right.y, item.expected_y);
    }

}

TEST(PositionedStones, PositionBCoordinates)
{
    constexpr float feet_to_meters = 1.0f / 3.28084f;

    auto const center = dc::coordinate::GetHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesType::kCenterGuard
    );
    EXPECT_FLOAT_EQ(center.x, 0.f);
    EXPECT_FLOAT_EQ(center.y, dc::coordinate::kTeeLineY + 2.0f * feet_to_meters - dc::Stone::kRadius);

    auto const left = dc::coordinate::GetHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesType::kPowerPlayLeft
    );
    EXPECT_FLOAT_EQ(left.x, -4.0f * feet_to_meters);
    EXPECT_FLOAT_EQ(left.y, dc::coordinate::kTeeLineY - dc::Stone::kRadius);

    auto const right = dc::coordinate::GetHammerTeamPositionedStonePoint(
        dc::rules::PositionedStonesType::kPowerPlayRight
    );
    EXPECT_FLOAT_EQ(right.x, 4.0f * feet_to_meters);
    EXPECT_FLOAT_EQ(right.y, dc::coordinate::kTeeLineY - dc::Stone::kRadius);

    auto const default_pt = dc::coordinate::GetHammerTeamPositionedStonePoint();
    EXPECT_FLOAT_EQ(default_pt.x, center.x);
    EXPECT_FLOAT_EQ(default_pt.y, center.y);
}

TEST(Json, GameSettingToJson)
{
    dc::GameSetting v;
    v.sheet_width = 5.f;
    v.thinking_time = dc::TeamValue<std::chrono::milliseconds>(
        std::chrono::milliseconds(1'525), std::chrono::milliseconds(23'456)
    );
    v.extra_end_thinking_time = dc::TeamValue<std::chrono::milliseconds>(
        std::chrono::milliseconds(525), std::chrono::milliseconds(3'456)
    );

    json const j = v;
    EXPECT_FALSE(j.contains("max_end"));
    EXPECT_EQ(j.at("sheet_width").get<float>(), v.sheet_width);
    EXPECT_EQ(j.at("thinking_time").at("team0").get<std::chrono::milliseconds>(), v.thinking_time[dc::Team::k0]);
    EXPECT_EQ(j.at("thinking_time").at("team1").get<std::chrono::milliseconds>(), v.thinking_time[dc::Team::k1]);
    EXPECT_EQ(j.at("extra_end_thinking_time").at("team0").get<std::chrono::milliseconds>(), v.extra_end_thinking_time[dc::Team::k0]);
    EXPECT_EQ(j.at("extra_end_thinking_time").at("team1").get<std::chrono::milliseconds>(), v.extra_end_thinking_time[dc::Team::k1]);
}

TEST(Json, GameSettingFromJson)
{
    json const j{
        { "max_end", 10 },
        { "sheet_width", 4.5 },
        { "thinking_time", {
            { "team0", 1.234 },
            { "team1", 5.678 },
        }},
        { "extra_end_thinking_time", {
            { "team0", 0.234 },
            { "team1", 0.678 },
        }}
    };

    dc::GameSetting v;
    j.get_to(v);
    EXPECT_EQ(v.sheet_width, 4.5f);
    EXPECT_EQ(v.thinking_time[dc::Team::k0], std::chrono::milliseconds(1234));
    EXPECT_EQ(v.thinking_time[dc::Team::k1], std::chrono::milliseconds(5678));
    EXPECT_EQ(v.extra_end_thinking_time[dc::Team::k0], std::chrono::milliseconds(234));
    EXPECT_EQ(v.extra_end_thinking_time[dc::Team::k1], std::chrono::milliseconds(678));
}

TEST(Json, GameSettingEndLimit)
{
    json j{
        { "sheet_width", 4.5 },
        { "thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }},
        { "extra_end_thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }}
    };

    dc::GameSetting v;
    v.end_limit = 3;
    j.get_to(v);
    EXPECT_EQ(v.end_limit, 255);

    j["end_limit"] = 12;
    j.get_to(v);
    EXPECT_EQ(v.end_limit, 12);

    json const j_out = v;
    EXPECT_EQ(j_out.at("end_limit").get<int>(), 12);
}

TEST(Json, GameResultReasonToJson)
{
    json const j_score = dc::GameResult::Reason::kScore;
    json const j_concede = dc::GameResult::Reason::kConcede;
    json const j_time_limit = dc::GameResult::Reason::kTimeLimit;
    json const j_draw = dc::GameResult::Reason::kDraw;
    json const j_disconnection = dc::GameResult::Reason::kDisconnection;
    EXPECT_EQ(j_score.get<std::string>(), "score");
    EXPECT_EQ(j_concede.get<std::string>(), "concede");
    EXPECT_EQ(j_time_limit.get<std::string>(), "time_limit");
    EXPECT_EQ(j_draw.get<std::string>(), "draw");
    EXPECT_EQ(j_disconnection.get<std::string>(), "disconnection");
}

TEST(Json, GameResultReasonFromJson)
{
    json const j_score = "score";
    json const j_concede = "concede";
    json const j_time_limit = "time_limit";
    json const j_draw = "draw";
    json const j_disconnection = "disconnection";

    dc::GameResult::Reason v_score, v_concede, v_time_limit, v_draw, v_disconnection;
    j_score.get_to(v_score);
    j_concede.get_to(v_concede);
    j_time_limit.get_to(v_time_limit);
    j_draw.get_to(v_draw);
    j_disconnection.get_to(v_disconnection);

    EXPECT_EQ(v_score, dc::GameResult::Reason::kScore);
    EXPECT_EQ(v_concede, dc::GameResult::Reason::kConcede);
    EXPECT_EQ(v_time_limit, dc::GameResult::Reason::kTimeLimit);
    EXPECT_EQ(v_draw, dc::GameResult::Reason::kDraw);
    EXPECT_EQ(v_disconnection, dc::GameResult::Reason::kDisconnection);
}

TEST(Json, GameResultToJson)
{
    dc::GameResult v;
    v.winner = dc::Team::k1;
    v.reason = dc::GameResult::Reason::kScore;
    json const j = v;
    EXPECT_EQ(j["winner"].get<std::string>(), "team1");
    EXPECT_EQ(j["reason"].get<std::string>(), "score");
}

TEST(Json, GameResultFromJson)
{
    json const j{
        { "winner", "team0" },
        { "reason", "draw" }
    };
    dc::GameResult v;
    j.get_to(v);
    EXPECT_EQ(v.winner, dc::Team::k0);
    EXPECT_EQ(v.reason, dc::GameResult::Reason::kDraw);
}

TEST(Json, GameStateToJson)
{
    dc::GameSetting setting;
    constexpr std::uint8_t max_end = 10;
    setting.sheet_width = 4.75f;
    setting.thinking_time = dc::TeamValue<std::chrono::milliseconds>(
        std::chrono::milliseconds(1'525), std::chrono::milliseconds(23'456)
    );
    setting.extra_end_thinking_time = dc::TeamValue<std::chrono::milliseconds>(
        std::chrono::milliseconds(525), std::chrono::milliseconds(3'456)
    );

    auto stones0 = std::array<std::optional<dc::Stone>, 8>{};
    auto stones1 = std::array<std::optional<dc::Stone>, 8>{};
    stones0[0] = std::make_optional<dc::Stone>(dc::Vector2(1.5f, 2.5f), 3.5f);
    auto stones = dc::StoneCoordinate(std::array<std::array<std::optional<dc::Stone>, 8>, 2>{ stones0, stones1 });

    auto scores0 = std::vector<std::optional<std::uint8_t>>(max_end + 1, std::nullopt);
    auto scores1 = std::vector<std::optional<std::uint8_t>>(max_end + 1, std::nullopt);
    scores0[0] = 2;
    scores1[0] = 0;
    auto scores = dc::GameScores(std::array<std::vector<std::optional<std::uint8_t>>, 2>{ scores0, scores1 });
    auto state = dc::GameState(max_end, 1, 10, dc::Team::k0, std::move(stones), std::move(scores), setting.thinking_time);

    json const j = state;
    EXPECT_EQ(j.at("end").get<std::uint8_t>(), state.GetEnd());
    EXPECT_EQ(j.at("shot").get<std::uint8_t>(), state.GetShot());
    EXPECT_EQ(j.at("hammer").get<std::string>(), "team0");
    EXPECT_EQ(j.at("stones").at("team0")[0].at("position").at("x").get<float>(), 1.5f);
    EXPECT_EQ(j.at("stones").at("team0")[0].at("position").at("y").get<float>(), 2.5f);
    EXPECT_EQ(j.at("stones").at("team0")[0].at("angle").get<float>(), 3.5f);
    EXPECT_TRUE(j.at("stones").at("team1")[0].is_null());
    EXPECT_EQ(j.at("scores").at("team0").size(), max_end + 1);
    EXPECT_EQ(j.at("scores").at("team1").size(), max_end + 1);
    EXPECT_EQ(j.at("scores").at("team0")[0].get<std::uint8_t>(), 2);
    EXPECT_EQ(j.at("scores").at("team1")[0].get<std::uint8_t>(), 0);
    EXPECT_TRUE(j.at("scores").at("team0")[1].is_null());
    EXPECT_TRUE(j.at("scores").at("team1")[1].is_null());
    EXPECT_EQ(j.at("thinking_time_remaining").at("team0").get<std::chrono::milliseconds>(), state.GetThinkingTimeRemaining()[dc::Team::k0]);
    EXPECT_EQ(j.at("thinking_time_remaining").at("team1").get<std::chrono::milliseconds>(), state.GetThinkingTimeRemaining()[dc::Team::k1]);
    EXPECT_TRUE(j.at("game_result").is_null());
}

TEST(Json, GameStateFromJson)
{
    // 8 end game
    json const j{
        { "end", 1 },
        { "shot", 3 },
        { "hammer", "team0" },
        { "stones", {
            { "team0", {
                {
                    { "position", {
                        { "x", 1.1f },
                        { "y", 1.2f }
                    }},
                    { "angle", 1.3f }
                },
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr
            }},
            { "team1", {
                {
                    { "position", {
                        { "x", 2.1f },
                        { "y", 2.2f }
                    }},
                    { "angle", 2.3f }
                },
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                nullptr
            }}
        }},
        { "scores", {
            { "team0", json::array({
                0, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
            })},
            { "team1", json::array({
                3, 0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
            })}
        }},
        { "thinking_time_remaining", {
            { "team0", 10.5 },
            { "team1", 9.5 }
        }},
        { "game_result", {
            { "winner", "team0" },
            { "reason", "concede" }
        }}
    };

    dc::GameState state(8);
    j.get_to(state);

    EXPECT_EQ(state.GetEnd(), 1);
    EXPECT_EQ(state.GetShot(), 3);
    EXPECT_EQ(state.GetHammer(), dc::Team::k0);

    EXPECT_EQ(state.GetStones()[dc::Team::k0][0]->position.x, 1.1f);
    EXPECT_EQ(state.GetStones()[dc::Team::k0][0]->position.y, 1.2f);
    EXPECT_EQ(state.GetStones()[dc::Team::k0][0]->angle, 1.3f);
    EXPECT_FALSE(state.GetStones()[dc::Team::k0][1].has_value());
    EXPECT_EQ(state.GetStones()[dc::Team::k1][0]->position.x, 2.1f);
    EXPECT_EQ(state.GetStones()[dc::Team::k1][0]->position.y, 2.2f);
    EXPECT_EQ(state.GetStones()[dc::Team::k1][0]->angle, 2.3f);
    EXPECT_FALSE(state.GetStones()[dc::Team::k1][1].has_value());

    EXPECT_EQ(state.GetScores()[dc::Team::k0].size(), 9);
    EXPECT_EQ(state.GetScores()[dc::Team::k1].size(), 9);
    EXPECT_EQ(state.GetScores()[dc::Team::k0][0].value(), 0);
    EXPECT_EQ(state.GetScores()[dc::Team::k1][0].value(), 3);
    EXPECT_EQ(state.GetScores()[dc::Team::k0][1].value(), 0);
    EXPECT_EQ(state.GetScores()[dc::Team::k1][1].value(), 0);

    EXPECT_EQ(state.GetThinkingTimeRemaining()[dc::Team::k0], std::chrono::milliseconds(10'500));
    EXPECT_EQ(state.GetThinkingTimeRemaining()[dc::Team::k1], std::chrono::milliseconds(9'500));
    EXPECT_EQ(state.GetGameResult()->winner, dc::Team::k0);
    EXPECT_EQ(state.GetGameResult()->reason, dc::GameResult::Reason::kConcede);
}

namespace {

dc::StoneCoordinate MakeGameStateStones(std::optional<std::uint8_t> team0_stone_number,
                                        std::optional<std::uint8_t> team1_stone_number)
{
    auto team0 = std::array<std::optional<dc::Stone>, 8>{};
    auto team1 = std::array<std::optional<dc::Stone>, 8>{};
    if (team0_stone_number.has_value())
        team0[team0_stone_number.value()] = dc::Stone(dc::Vector2(1.f, 2.f), 3.f);
    if (team1_stone_number.has_value())
        team1[team1_stone_number.value()] = dc::Stone(dc::Vector2(4.f, 5.f), 6.f);
    return dc::StoneCoordinate({ team0, team1 });
}

} // unnamed namespace

TEST(GameState, ReservedStoneNumbersDefaultDoesNotExemptStones)
{
    EXPECT_THROW(dc::GameState(8, 0, 0, dc::Team::k1, MakeGameStateStones(0, std::nullopt)), std::invalid_argument);
    EXPECT_THROW(dc::GameState(8, 0, 0, dc::Team::k1, MakeGameStateStones(std::nullopt, 0)), std::invalid_argument);
    EXPECT_NO_THROW(dc::GameState(8, 0, 1, dc::Team::k1, MakeGameStateStones(0, std::nullopt)));
    EXPECT_THROW(dc::GameState(8, 0, 1, dc::Team::k1, MakeGameStateStones(std::nullopt, 0)), std::invalid_argument);
    EXPECT_THROW(dc::GameState(8, 0, 1, dc::Team::k1, MakeGameStateStones(1, std::nullopt)), std::invalid_argument);
    EXPECT_THROW(dc::GameState(8, 0, 1, dc::Team::k1, MakeGameStateStones(std::nullopt, 1)), std::invalid_argument);
}

TEST(GameState, StoneCountValidationFollowsThrowOrder)
{
    for (dc::Team hammer : { dc::Team::k0, dc::Team::k1 }) {
        auto const non_hammer = dc::GetOpponentTeam(hammer);
        for (std::uint8_t shot = 0; shot <= dc::StoneCoordinate::kStoneMax; ++shot) {
            auto const check_boundary = [&](dc::Team team, int allowed_stone_count) {
                auto make_stones = [&](int stone_number) {
                    auto const number = static_cast<std::uint8_t>(stone_number);
                    return team == dc::Team::k0
                        ? MakeGameStateStones(number, std::nullopt)
                        : MakeGameStateStones(std::nullopt, number);
                };

                if (allowed_stone_count > 0) {
                    EXPECT_NO_THROW(dc::GameState(
                        8, 0, shot, hammer, make_stones(allowed_stone_count - 1)
                    ));
                }
                if (allowed_stone_count < dc::StoneCoordinate::kStoneMax / 2) {
                    EXPECT_THROW(
                        dc::GameState(8, 0, shot, hammer, make_stones(allowed_stone_count)),
                        std::invalid_argument
                    );
                }
            };

            check_boundary(non_hammer, (shot + 1) / 2);
            check_boundary(hammer, shot / 2);
        }
    }
}

TEST(GameState, ReservedStoneNumbersExemptExactStones)
{
    auto const reserved = dc::TeamValue<std::optional<std::uint8_t>>(6, 7);

    EXPECT_NO_THROW(dc::GameState(
        8, 0, 0, dc::Team::k1, MakeGameStateStones(6, 7), std::nullopt, std::nullopt, std::nullopt, reserved
    ));
    EXPECT_NO_THROW(dc::GameState(
        8, 0, 5, dc::Team::k1, MakeGameStateStones(6, 7), std::nullopt, std::nullopt, std::nullopt, reserved
    ));
}

TEST(GameState, ReservedStoneNumbersDoNotExemptOtherStones)
{
    auto const reserved = dc::TeamValue<std::optional<std::uint8_t>>(7, 7);

    EXPECT_THROW(
        dc::GameState(
            8, 0, 0, dc::Team::k1, MakeGameStateStones(6, 7), std::nullopt, std::nullopt, std::nullopt, reserved
        ),
        std::invalid_argument
    );
}

TEST(GameState, ReservedStoneNumbersDoNotAffectOtherValidation)
{
    auto const reserved = dc::TeamValue<std::optional<std::uint8_t>>(7, 7);

    EXPECT_THROW(
        dc::GameState(8, 255, 0, dc::Team::k1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, reserved),
        std::invalid_argument
    );
    EXPECT_THROW(
        dc::GameState(8, 0, 17, dc::Team::k1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, reserved),
        std::invalid_argument
    );
    EXPECT_THROW(
        dc::GameState(8, 0, 0, dc::Team::k1, std::nullopt, dc::GameScores(7), std::nullopt, std::nullopt, reserved),
        std::invalid_argument
    );
    EXPECT_THROW(
        dc::GameState(8, 2, 0, dc::Team::k1, std::nullopt, dc::GameScores(8), std::nullopt, std::nullopt, reserved),
        std::invalid_argument
    );

    auto scores0 = std::vector<std::optional<std::uint8_t>>(9, std::nullopt);
    auto scores1 = std::vector<std::optional<std::uint8_t>>(9, std::nullopt);
    scores0[0] = 0;
    scores1[0] = 0;
    auto scores = dc::GameScores({ scores0, scores1 });
    EXPECT_THROW(
        dc::GameState(8, 0, 0, dc::Team::k1, std::nullopt, std::move(scores), std::nullopt, std::nullopt, reserved),
        std::invalid_argument
    );
}

TEST(GameState, ReservedStoneNumbersMustBeInRange)
{
    for (dc::Team team : { dc::Team::k0, dc::Team::k1 }) {
        for (std::uint8_t invalid_number : { std::uint8_t(8), std::uint8_t(255) }) {
            auto reserved = dc::TeamValue<std::optional<std::uint8_t>>{};
            reserved = team == dc::Team::k0
                ? dc::TeamValue<std::optional<std::uint8_t>>(invalid_number, std::nullopt)
                : dc::TeamValue<std::optional<std::uint8_t>>(std::nullopt, invalid_number);
            EXPECT_THROW(
                dc::GameState(8, 0, 0, dc::Team::k1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, reserved),
                std::invalid_argument
            );
        }

        auto reserved = team == dc::Team::k0
            ? dc::TeamValue<std::optional<std::uint8_t>>(7, std::nullopt)
            : dc::TeamValue<std::optional<std::uint8_t>>(std::nullopt, 7);
        EXPECT_NO_THROW(dc::GameState(
            8, 0, 0, dc::Team::k1, std::nullopt, std::nullopt, std::nullopt, std::nullopt, reserved
        ));
    }
}

TEST(GameState, OmittedScoresUseMaxEnd)
{
    dc::GameState const state(8);
    EXPECT_EQ(state.GetScores().GetEndCount(), 8);
    EXPECT_EQ(state.GetScores()[dc::Team::k0].size(), 9u);
    EXPECT_EQ(state.GetScores()[dc::Team::k1].size(), 9u);
}

TEST(GameState, ScoresJsonRoundTripKeepsEndCount)
{
    json const j = dc::GameState(8);
    dc::GameState state(10);
    j.get_to(state);
    EXPECT_EQ(state.GetScores().GetEndCount(), 8);
}

TEST(GameState, ScoresJsonRejectsInvalidArrays)
{
    json j = dc::GameState(8);
    j["scores"]["team0"] = json::array();
    j["scores"]["team1"] = json::array();
    dc::GameState state(8);
    EXPECT_THROW(j.get_to(state), std::invalid_argument);
}

TEST(GameState, ReservedStoneNumbersJsonRoundTrip)
{
    auto const reserved = dc::TeamValue<std::optional<std::uint8_t>>(6, 7);
    auto const state = dc::GameState(
        8,
        0,
        0,
        dc::Team::k1,
        MakeGameStateStones(6, 7),
        dc::GameScores(8),
        std::nullopt,
        std::nullopt,
        reserved
    );

    json const j = state;
    dc::GameState round_tripped(8);
    EXPECT_NO_THROW(j.get_to(round_tripped));
    EXPECT_EQ(round_tripped.GetReservedStoneNumbers()[dc::Team::k0], reserved[dc::Team::k0]);
    EXPECT_EQ(round_tripped.GetReservedStoneNumbers()[dc::Team::k1], reserved[dc::Team::k1]);
    ASSERT_TRUE(round_tripped.GetStones()[dc::Team::k0][6].has_value());
    ASSERT_TRUE(round_tripped.GetStones()[dc::Team::k1][7].has_value());
    EXPECT_FLOAT_EQ(round_tripped.GetStones()[dc::Team::k0][6]->position.x, 1.f);
    EXPECT_FLOAT_EQ(round_tripped.GetStones()[dc::Team::k1][7]->position.x, 4.f);
}

TEST(GameState, MissingReservedStoneNumbersJsonDefaultsToNoExemption)
{
    json const j{
        { "end", 0 },
        { "shot", 0 },
        { "hammer", "team1" },
        { "stones", dc::StoneCoordinate() },
        { "scores", dc::GameScores(8) },
        { "thinking_time_remaining", dc::TeamValue<std::chrono::milliseconds>() },
        { "game_result", nullptr }
    };
    dc::GameState state(8);

    EXPECT_NO_THROW(j.get_to(state));
    EXPECT_FALSE(state.GetReservedStoneNumbers()[dc::Team::k0].has_value());
    EXPECT_FALSE(state.GetReservedStoneNumbers()[dc::Team::k1].has_value());
}

TEST(GameState, ReservedStoneNumbersJsonRejectsOutOfRangeValues)
{
    dc::GameState state(8);
    json j = state;
    j["reserved_stone_numbers"]["team0"] = 256;

    EXPECT_THROW(j.get_to(state), std::invalid_argument);
}

namespace {

template <typename Enum>
void ExpectStrictEnumJson(std::initializer_list<std::pair<Enum, char const*>> valid)
{
    for (auto const& [value, token] : valid) {
        json const j = value;
        EXPECT_EQ(j, json(token));
        EXPECT_EQ(j.get<Enum>(), value);
    }

    std::string const first_token = valid.begin()->second;
    std::string wrong_case = first_token;
    wrong_case[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(wrong_case[0])));

    EXPECT_THROW(json("invalid").get<Enum>(), std::invalid_argument);
    EXPECT_THROW(json("").get<Enum>(), std::invalid_argument);
    EXPECT_THROW(json(wrong_case).get<Enum>(), std::invalid_argument);
    EXPECT_THROW(json(0).get<Enum>(), std::invalid_argument);
    EXPECT_THROW(json(true).get<Enum>(), std::invalid_argument);
    EXPECT_THROW(json(nullptr).get<Enum>(), std::invalid_argument);

    // Every value of a bool-based enum is valid, so there is nothing to reject on output.
    if constexpr (!std::is_same_v<std::underlying_type_t<Enum>, bool>) {
        json j_out;
        EXPECT_THROW(j_out = static_cast<Enum>(0x7f), std::invalid_argument);
    }
}

} // unnamed namespace

TEST(Json, StrictEnumTeam)
{
    ExpectStrictEnumJson<dc::Team>({
        { dc::Team::k0, "team0" },
        { dc::Team::k1, "team1" }
    });
}

TEST(Json, StrictEnumGameResultReason)
{
    ExpectStrictEnumJson<dc::GameResult::Reason>({
        { dc::GameResult::Reason::kScore, "score" },
        { dc::GameResult::Reason::kConcede, "concede" },
        { dc::GameResult::Reason::kTimeLimit, "time_limit" },
        { dc::GameResult::Reason::kDraw, "draw" },
        { dc::GameResult::Reason::kDisconnection, "disconnection" }
    });
}

TEST(Json, StrictEnumGender)
{
    ExpectStrictEnumJson<dc::players::Gender>({
        { dc::players::Gender::kMale, "male" },
        { dc::players::Gender::kFemale, "female" },
        { dc::players::Gender::kUnknown, "unknown" }
    });
}

TEST(Json, StrictEnumGameRuleType)
{
    ExpectStrictEnumJson<dc::GameRuleType>({
        { dc::GameRuleType::kStandard, "standard" },
        { dc::GameRuleType::kMixed, "mixed" },
        { dc::GameRuleType::kMixedDoubles, "mixed_doubles" }
    });
}

TEST(Json, StrictEnumAdditionalRuleTypes)
{
    ExpectStrictEnumJson<dc::rules::AdditionalRuleTypes>({
        { dc::rules::AdditionalRuleTypes::kFreeGuardZone, "free_guard_zone" },
        { dc::rules::AdditionalRuleTypes::kNoTickShot, "no_tick_shot" }
    });
}

TEST(Json, StrictEnumPositionedStonesPattern)
{
    ExpectStrictEnumJson<dc::rules::PositionedStonesPattern>({
        { dc::rules::PositionedStonesPattern::kPosition1Front, "position1_front" },
        { dc::rules::PositionedStonesPattern::kPosition1Back, "position1_back" },
        { dc::rules::PositionedStonesPattern::kPosition2Front, "position2_front" },
        { dc::rules::PositionedStonesPattern::kPosition2Back, "position2_back" },
        { dc::rules::PositionedStonesPattern::kPosition3Front, "position3_front" },
        { dc::rules::PositionedStonesPattern::kPosition3Back, "position3_back" },
        { dc::rules::PositionedStonesPattern::kPosition4Front, "position4_front" },
        { dc::rules::PositionedStonesPattern::kPosition4Back, "position4_back" }
    });
}

TEST(GameScores, JsonRoundTripKeepsEndCount)
{
    std::vector<std::optional<std::uint8_t>> scores0(11, std::nullopt);
    std::vector<std::optional<std::uint8_t>> scores1(11, std::nullopt);
    scores0[0] = 1;
    scores1[0] = 0;
    dc::GameScores const scores({ scores0, scores1 });
    ASSERT_EQ(scores.GetEndCount(), 10);

    json const j = scores;
    EXPECT_EQ(j.at("team0").size(), 11u);
    EXPECT_EQ(j.at("team1").size(), 11u);

    auto const restored = j.get<dc::GameScores>();
    EXPECT_EQ(restored.GetEndCount(), 10);
    EXPECT_EQ(restored[dc::Team::k0], scores0);
    EXPECT_EQ(restored[dc::Team::k1], scores1);
}

TEST(GameScores, JsonRejectsInvalidArrays)
{
    // mismatched sizes
    json const mismatched{
        { "team0", json::array({ 0, nullptr, nullptr }) },
        { "team1", json::array({ 0, nullptr }) }
    };
    EXPECT_THROW(mismatched.get<dc::GameScores>(), std::invalid_argument);

    // too short
    json const too_short{
        { "team0", json::array({ 0 }) },
        { "team1", json::array({ 0 }) }
    };
    EXPECT_THROW(too_short.get<dc::GameScores>(), std::invalid_argument);

    // empty
    json const empty{
        { "team0", json::array() },
        { "team1", json::array() }
    };
    EXPECT_THROW(empty.get<dc::GameScores>(), std::invalid_argument);

    // a determined score after an undetermined one
    json const gap{
        { "team0", json::array({ 0, nullptr, 1 }) },
        { "team1", json::array({ 0, nullptr, 1 }) }
    };
    EXPECT_THROW(gap.get<dc::GameScores>(), std::invalid_argument);

    // determined for only one team
    json const one_sided{
        { "team0", json::array({ 0, 1, nullptr }) },
        { "team1", json::array({ 0, nullptr, nullptr }) }
    };
    EXPECT_THROW(one_sided.get<dc::GameScores>(), std::invalid_argument);
}

TEST(GameState, EndBeyondMaxEndWithResultDoesNotIndexOutOfRange)
{
    constexpr std::uint8_t max_end = 2;
    std::vector<std::optional<std::uint8_t>> scores0 = { 1, 0, 2 };
    std::vector<std::optional<std::uint8_t>> scores1 = { 0, 1, 0 };
    dc::GameResult result;
    result.winner = dc::Team::k0;
    result.reason = dc::GameResult::Reason::kScore;

    for (std::uint8_t end : { std::uint8_t(max_end), std::uint8_t(max_end + 1), std::uint8_t(max_end + 2) }) {
        EXPECT_NO_THROW(dc::GameState(
            max_end, end, 0, dc::Team::k1, std::nullopt, dc::GameScores({ scores0, scores1 }), std::nullopt, result
        )) << "end=" << int(end);
    }

    // an undetermined slot still inside the checked range must be rejected
    std::vector<std::optional<std::uint8_t>> partial0 = { 1, 0, std::nullopt };
    std::vector<std::optional<std::uint8_t>> partial1 = { 0, 1, std::nullopt };
    EXPECT_THROW(
        dc::GameState(max_end, max_end + 1, 0, dc::Team::k1, std::nullopt, dc::GameScores({ partial0, partial1 }), std::nullopt, result),
        std::invalid_argument
    );
}

namespace {

std::vector<json> InvalidUint8Values()
{
    return { json(256), json(-1), json(3.5), json(1.0), json(true), json(1e300), json("1"), json(nullptr) };
}

} // unnamed namespace

TEST(Json, CheckedIntegerRangeAndType)
{
    for (std::uint8_t valid : { std::uint8_t(0), std::uint8_t(1), std::uint8_t(255) }) {
        EXPECT_EQ(dc::json_util::GetCheckedInteger<std::uint8_t>(json(valid), "v"), valid);
        EXPECT_EQ(dc::json_util::GetCheckedInteger<std::uint8_t>(json(static_cast<int>(valid)), "v"), valid);
    }
    for (auto const& invalid : InvalidUint8Values()) {
        EXPECT_THROW(dc::json_util::GetCheckedInteger<std::uint8_t>(invalid, "v"), std::invalid_argument) << invalid.dump();
    }

    EXPECT_EQ(dc::json_util::GetCheckedInteger<std::int8_t>(json(-128), "v"), -128);
    EXPECT_EQ(dc::json_util::GetCheckedInteger<std::int8_t>(json(127), "v"), 127);
    EXPECT_THROW(dc::json_util::GetCheckedInteger<std::int8_t>(json(-129), "v"), std::invalid_argument);
    EXPECT_THROW(dc::json_util::GetCheckedInteger<std::int8_t>(json(128), "v"), std::invalid_argument);
}

TEST(Json, GameRuleRegularEndCountIsChecked)
{
    json j = dc::GameRule();
    for (auto const& invalid : InvalidUint8Values()) {
        j["regular_end_count"] = invalid;
        dc::GameRule v;
        EXPECT_THROW(j.get_to(v), std::invalid_argument) << invalid.dump();
    }
    for (int valid : { 0, 255 }) {
        j["regular_end_count"] = valid;
        dc::GameRule v;
        EXPECT_NO_THROW(j.get_to(v));
        EXPECT_EQ(v.regular_end_count, valid);
    }
}

TEST(Json, ShotRuleAppliedShotCountIsChecked)
{
    for (auto const& invalid : InvalidUint8Values()) {
        json const j{ { "applied_shot_count", invalid } };
        dc::rules::FreeGuardZoneRule fgz;
        dc::rules::NoTickShotRule nts;
        EXPECT_THROW(j.get_to(fgz), std::invalid_argument) << invalid.dump();
        EXPECT_THROW(j.get_to(nts), std::invalid_argument) << invalid.dump();
    }
    for (int valid : { 0, 255 }) {
        json const j{ { "applied_shot_count", valid } };
        dc::rules::FreeGuardZoneRule fgz;
        dc::rules::NoTickShotRule nts;
        EXPECT_NO_THROW(j.get_to(fgz));
        EXPECT_NO_THROW(j.get_to(nts));
        EXPECT_EQ(fgz.applied_shot_count, valid);
        EXPECT_EQ(nts.applied_shot_count, valid);
    }
}

TEST(Json, StoneIndexStoneIsChecked)
{
    for (auto const& invalid : InvalidUint8Values()) {
        json const j{ { "team", "team0" }, { "stone", invalid } };
        dc::StoneIndex v(dc::Team::k0, 0);
        EXPECT_THROW(j.get_to(v), std::invalid_argument) << invalid.dump();
    }
    json const j{ { "team", "team1" }, { "stone", 7 } };
    dc::StoneIndex v(dc::Team::k0, 0);
    EXPECT_NO_THROW(j.get_to(v));
    EXPECT_EQ(v.GetStoneNumber(), 7);
}

TEST(Json, GameSettingEndLimitIsChecked)
{
    json j{
        { "sheet_width", 4.5 },
        { "thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }},
        { "extra_end_thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }}
    };
    for (auto const& invalid : InvalidUint8Values()) {
        j["end_limit"] = invalid;
        dc::GameSetting v;
        EXPECT_THROW(j.get_to(v), std::invalid_argument) << invalid.dump();
    }
    for (int valid : { 0, 255 }) {
        j["end_limit"] = valid;
        dc::GameSetting v;
        EXPECT_NO_THROW(j.get_to(v));
        EXPECT_EQ(v.end_limit, valid);
    }
}

TEST(Json, GameStateIntegersAreChecked)
{
    dc::GameState state(8);
    for (char const* key : { "end", "shot" }) {
        for (auto const& invalid : InvalidUint8Values()) {
            json j = state;
            j[key] = invalid;
            EXPECT_THROW(j.get_to(state), std::invalid_argument) << key << " " << invalid.dump();
        }
    }
    for (auto const& invalid : InvalidUint8Values()) {
        if (invalid.is_null()) continue;  // null means "no reserved stone number"
        json j = state;
        j["reserved_stone_numbers"]["team1"] = invalid;
        EXPECT_THROW(j.get_to(state), std::invalid_argument) << invalid.dump();
    }
}

TEST(Json, GameScoresElementsAreChecked)
{
    for (auto const& invalid : InvalidUint8Values()) {
        if (invalid.is_null()) continue;  // null means "undetermined"
        json const j{
            { "team0", json::array({ invalid, nullptr, nullptr }) },
            { "team1", json::array({ 0, nullptr, nullptr }) }
        };
        EXPECT_THROW(j.get<dc::GameScores>(), std::invalid_argument) << invalid.dump();
    }

    json const not_array{
        { "team0", 1 },
        { "team1", json::array({ 0, nullptr }) }
    };
    EXPECT_THROW(not_array.get<dc::GameScores>(), std::invalid_argument);

    json const valid{
        { "team0", json::array({ 255, nullptr }) },
        { "team1", json::array({ 0, nullptr }) }
    };
    auto const scores = valid.get<dc::GameScores>();
    EXPECT_EQ(scores[dc::Team::k0][0], 255);
}

TEST(Json, MillisecondsRejectsInvalidDurations)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    for (double invalid : { nan, inf, -inf, 1e300, -1e300, 1e13, -1e13, 9.007199254740992e12 }) {
        json const j = invalid;
        EXPECT_THROW(j.get<std::chrono::milliseconds>(), std::invalid_argument) << invalid;
    }
    EXPECT_THROW(json("1").get<std::chrono::milliseconds>(), nlohmann::json::type_error);

    EXPECT_EQ(json(0).get<std::chrono::milliseconds>().count(), 0);
    EXPECT_EQ(json(0.0).get<std::chrono::milliseconds>().count(), 0);
    EXPECT_EQ(json(3600).get<std::chrono::milliseconds>().count(), 3'600'000);
}

TEST(Json, MillisecondsAcceptsNegativeDurations)
{
    EXPECT_EQ(json(-1.5).get<std::chrono::milliseconds>().count(), -1500);
    EXPECT_EQ(json(-0.001).get<std::chrono::milliseconds>().count(), -1);

    std::chrono::milliseconds const v(-2'345);
    json const j = v;
    EXPECT_EQ(j.get<std::chrono::milliseconds>(), v);
}

TEST(Json, FloatsRejectNonFiniteAndOutOfRange)
{
    constexpr double inf = std::numeric_limits<double>::infinity();
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    for (double invalid : { nan, inf, -inf, 1e300, -1e39, 3.5e38 }) {
        json const vector2{ { "x", invalid }, { "y", 0.0 } };
        EXPECT_THROW(vector2.get<dc::Vector2>(), std::invalid_argument) << invalid;

        json const stone{ { "position", { { "x", 0.0 }, { "y", 0.0 } } }, { "angle", invalid } };
        EXPECT_THROW(stone.get<dc::Stone>(), std::invalid_argument) << invalid;

        json const shot{ { "translational_velocity", 1.0 }, { "angular_velocity", invalid }, { "release_angle", 0.0 } };
        EXPECT_THROW(shot.get<dc::moves::Shot>(), std::invalid_argument) << invalid;

        json setting{
            { "sheet_width", invalid },
            { "thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }},
            { "extra_end_thinking_time", { { "team0", 1.0 }, { "team1", 1.0 } }}
        };
        EXPECT_THROW(setting.get<dc::GameSetting>(), std::invalid_argument) << invalid;
    }

    json const bool_value{ { "x", true }, { "y", 0.0 } };
    EXPECT_THROW(bool_value.get<dc::Vector2>(), std::invalid_argument);

    json const max_value{ { "x", static_cast<double>(std::numeric_limits<float>::max()) }, { "y", -1.5 } };
    auto const v = max_value.get<dc::Vector2>();
    EXPECT_EQ(v.x, std::numeric_limits<float>::max());
    EXPECT_EQ(v.y, -1.5f);
}
