// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameRule

#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/game_rule_type.hpp"
#include "digitalcurling/team.hpp"
#include "digitalcurling/rules/i_additional_rule.hpp"
#include "digitalcurling/rules/free_guard_zone.hpp"
#include "digitalcurling/rules/no_tick_shot.hpp"
#include "digitalcurling/rules/positioned_stones.hpp"

namespace digitalcurling {

class StoneCoordinate;

/// @brief Game rules
struct GameRule {
public:
    /// @brief Maximum number of ends, including extra ends
    static constexpr std::uint8_t kEndLimit = 255;

    /// @brief Type of game rule
    GameRuleType type;

    /// @brief Number of ends excluding extra ends
    std::uint8_t regular_end_count = 10;

    /// @brief Whether to apply wheelchair rules
    /// @note
    /// This option currently has no effect on the game rules.
    /// It is intended to control whether sweeping is allowed in the future.
    bool is_wheelchair = false;

    /// @brief Pre-positioned stone pattern, or std::nullopt outside mixed doubles
    std::optional<rules::PositionedStonesPattern> positioned_stones;

    /// @brief Free guard zone rule
    std::optional<rules::FreeGuardZoneRule> free_guard_zone;

    /// @brief No-tick shot rule
    std::optional<rules::NoTickShotRule> no_tick_shot;

    /// @brief Constructor
    GameRule() : type(GameRuleType::kStandard), regular_end_count(10), is_wheelchair(false) {}

    /// @brief Constructor
    /// @param type Type of game rule
    /// @param is_wheelchair Whether wheelchair rules apply
    /// @param regular_end_count Number of ends excluding extra ends (0 selects the default)
    /// @param enable_free_guard_zone Whether to enable the free guard zone rule
    /// @param enable_no_tick_shot Whether to enable the no-tick shot rule
    /// @param positioned_stones Pre-positioned stone pattern, or std::nullopt outside mixed doubles
    GameRule(GameRuleType type, bool is_wheelchair, std::uint8_t regular_end_count = 0,
             bool enable_free_guard_zone = false, bool enable_no_tick_shot = false,
             std::optional<rules::PositionedStonesPattern> positioned_stones = std::nullopt)
        : type(type), regular_end_count(regular_end_count), is_wheelchair(is_wheelchair), positioned_stones(positioned_stones)
    {
        if (type == GameRuleType::kMixedDoubles && !positioned_stones.has_value())
            throw std::invalid_argument("mixed doubles requires positioned stones.");

        if (regular_end_count == 0) this->regular_end_count = type == GameRuleType::kMixedDoubles ? 8 : 10;
        if (enable_free_guard_zone) free_guard_zone = rules::FreeGuardZoneRule(type);
        if (enable_no_tick_shot) no_tick_shot = rules::NoTickShotRule(type);
    }

    /// @brief Checks whether the shot follows the rule
    /// @param[in] shot_in_end Shot number within the end
    /// @param[in] deliver Team that delivered the shot
    /// @param[in] before Board state before the shot
    /// @param[in] after Board state after the shot
    /// @return Type of rule violated, or `std::nullopt` if the shot follows the rules
    std::optional<rules::AdditionalRuleTypes> VerifyShot(std::uint8_t shot_in_end, Team deliver, StoneCoordinate before, StoneCoordinate after) const {
        const rules::IShotRule* rules[] = {
            free_guard_zone ? &*free_guard_zone : nullptr,
            no_tick_shot ? &*no_tick_shot : nullptr
        };

        for (const auto& rule : rules) {
            if (rule && !rule->ValidateShot(shot_in_end, deliver, before, after))
                return rule->GetType();
        }
        return std::nullopt;
    }
};


/// @cond Doxygen_Suppress
inline void to_json(nlohmann::json & j, GameRule const& v) {
    j["type"] = v.type;
    j["is_wheelchair"] = v.is_wheelchair;
    j["regular_end_count"] = v.regular_end_count;
    j["positioned_stones"] = v.positioned_stones;
    j["free_guard_zone"] = v.free_guard_zone;
    j["no_tick_shot"] = v.no_tick_shot;
};
inline void from_json(nlohmann::json const& j, GameRule & v) {
    j.at("type").get_to(v.type);
    j.at("is_wheelchair").get_to(v.is_wheelchair);
    v.regular_end_count = json_util::GetChecked<std::uint8_t>(j, "regular_end_count");
    try_get_to(j, "positioned_stones", v.positioned_stones, std::nullopt);
    try_get_to(j, "free_guard_zone", v.free_guard_zone, std::nullopt);
    try_get_to(j, "no_tick_shot", v.no_tick_shot, std::nullopt);
}
/// @endcond

} // namespace digitalcurling
