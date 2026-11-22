// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines FreeGuardZoneRule

#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/game_rule_type.hpp"
#include "digitalcurling/rules/i_additional_rule.hpp"

namespace digitalcurling::rules {


/// @brief Free guard zone rule
class FreeGuardZoneRule : public IShotRule {
public:
    // Reject legacy enable flags before they can convert to a shot count.
    template <typename T, std::enable_if_t<std::is_same_v<T, bool>, int> = 0>
    FreeGuardZoneRule(T) = delete;

    /// @brief Constructor (initializes the default shot count for the rule type)
    /// @param[in] type Rule type
    FreeGuardZoneRule(GameRuleType type = GameRuleType::kStandard) : IShotRule(type == GameRuleType::kMixedDoubles ? 3 : 5) { }

    /// @brief Constructor
    /// @param[in] applied_shot_count Number of shots to which the rule applies
    FreeGuardZoneRule(std::uint8_t applied_shot_count) : IShotRule(applied_shot_count) { }

    AdditionalRuleTypes GetType() const override { return AdditionalRuleTypes::kFreeGuardZone; }

    bool ValidateShot(std::uint8_t shot_in_end, Team deliver, StoneCoordinate before, StoneCoordinate after) const override {
        if (applied_shot_count != 0 && shot_in_end >= applied_shot_count) return true;

        static auto is_in_free_guard_zone = [](Stone stone) {
            return !stone.IsInHouse() && stone.position.y + Stone::kRadius < coordinate::kTeeLineY;
        };

        Team const opponent = GetOpponentTeam(deliver);
        auto before_stones = before.GetTeamStones(opponent);
        std::vector<StoneIndex> targets {};
        for (std::uint8_t i = 0; i < before_stones.size(); ++i) {
            if (!before_stones[i].has_value()) continue;

            auto stone = before_stones[i].value();
            if (is_in_free_guard_zone(stone)) {
                targets.push_back({opponent, i});
            }
        }

        for (auto const& target : targets) {
            auto stone = after[target];
            if (!stone.has_value() || !is_in_free_guard_zone(stone.value())) {
                return false;
            }
        }
        return true;
    }
};


/// @cond Doxygen_Suppress
inline void to_json(nlohmann::json & j, digitalcurling::rules::FreeGuardZoneRule const& v) {
    j["applied_shot_count"] = v.applied_shot_count;
}
inline void from_json(nlohmann::json const& j, digitalcurling::rules::FreeGuardZoneRule & v) {
    v.applied_shot_count = digitalcurling::json_util::GetChecked<std::uint8_t>(j, "applied_shot_count");
}
/// @endcond

} // namespace digitalcurling::rules
