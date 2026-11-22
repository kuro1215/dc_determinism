// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameScores

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/game_rule.hpp"
#include "digitalcurling/team.hpp"

namespace digitalcurling {


/// @brief Game score information
///
/// An undetermined score or a missing extra end is represented by `std::nullopt`.
///
/// Example for a 10-end game
/// ```cpp
/// GameScores scores(10);
/// scores[Team::k0][0];  // Team 0's score in the first end
/// scores[Team::k0][9];  // Team 0's score in the tenth end
/// scores[Team::k1][10]; // Team 1's score in the extra end
/// ```
struct GameScores : public TeamValue<std::vector<std::optional<std::uint8_t>>> {
private:
    /// @brief Number of ends
    std::uint8_t max_end_;

public:
    /// @brief Constructor
    GameScores() : max_end_(0) { };

    /// @brief Constructor
    /// @param[in] max_end Number of ends, excluding extra ends
    GameScores(std::uint8_t max_end) :
        max_end_(max_end),
        TeamValue<std::vector<std::optional<std::uint8_t>>>(std::vector<std::optional<std::uint8_t>>(max_end + 1))
    {
        if (max_end >= GameRule::kEndLimit)
            throw std::out_of_range("max_end must be less than GameRule::kEndLimit.");
    }

    /// @brief Constructor
    /// @param[in] scores Scores (team0, team1)
    /// @note The score arrays must have the same size.
    /// @note The number of stored scores (other than `std::nullopt`) for the hammer team must equal or be one less than that of the other team.
    GameScores(std::array<std::vector<std::optional<std::uint8_t>>, 2> scores)
        : TeamValue<std::vector<std::optional<std::uint8_t>>>(std::move(scores[0]), std::move(scores[1]))
    {

        auto const& team0 = (*this)[Team::k0];
        auto const& team1 = (*this)[Team::k1];
        if (team0.size() != team1.size() || team0.size() < 2 || team0.size() > 255)
            throw std::invalid_argument("invalid scores data: size of team0 and team1 must be same and must be between 2 and 255.");

        bool none = false;
        for (int i = 0; i < team0.size(); i++) {
            if (team0[i].has_value() != team1[i].has_value() || (team0[i].has_value() && none))
                throw std::invalid_argument("invalid scores data: scores for complete ends must be determined and scores for incomplete ends must be undetermined.");
            none = none || !team0[i].has_value();
        }
        max_end_ = static_cast<std::uint8_t>(team0.size() - 1);

    }

    /// @brief Gets a team's score
    /// @param[in] team Team
    /// @param[in] end End
    /// @return Score, or `std::nullopt` if undetermined
    /// @exception std::out_of_range If `end` is out of range
    std::optional<std::uint8_t> GetEndScore(Team team, std::uint8_t end) const {
        return (*this)[team].at(end);
    }
    /// @brief Gets a team's score in the extra end
    /// @param[in] team Team
    /// @return Score, or `std::nullopt` if undetermined
    std::optional<std::uint8_t> GetExtraEndScore(Team team) const {
        return GetEndScore(team, max_end_);
    }

    /// @brief Gets the number of ends, excluding extra ends
    /// @return Number of ends
    std::uint8_t GetEndCount() const noexcept {
        return max_end_;
    }

    /// @brief Gets the total scores
    /// @return Total scores (team0, team1)
    TeamValue<std::uint16_t> GetTotalScore() const {
        return { GetTotalScore(Team::k0), GetTotalScore(Team::k1) };
    }
    /// @brief Gets a team's total score
    /// @param[in] team Team
    /// @return Total score
    std::uint16_t GetTotalScore(Team team) const {
        std::uint16_t total = 0;
        for (auto const& score : (*this)[team]) {
            if (!score.has_value()) break;
            total += score.value();
        }
        return total;
    }
};


/// @cond Doxygen_Suppress
// json
namespace json_util {

inline std::vector<std::optional<std::uint8_t>> GetCheckedScoreArray(nlohmann::json const& j, char const* key) {
    auto const& array = j.at(key);
    if (!array.is_array())
        throw std::invalid_argument(std::string("invalid scores data: ") + key + " must be an array.");

    std::vector<std::optional<std::uint8_t>> scores;
    scores.reserve(array.size());
    for (auto const& element : array) {
        if (element.is_null()) {
            scores.emplace_back(std::nullopt);
        } else {
            scores.emplace_back(GetCheckedInteger<std::uint8_t>(element, "score"));
        }
    }
    return scores;
}

} // namespace json_util

// These non-template overloads take precedence over the TeamValue template, so that
// the arrays always go through the validating GameScores constructor.
inline void to_json(nlohmann::json& j, GameScores const& v) {
    j = nlohmann::json{
        {"team0", v[Team::k0]},
        {"team1", v[Team::k1]}
    };
}
inline void from_json(nlohmann::json const& j, GameScores& v) {
    v = GameScores(std::array<std::vector<std::optional<std::uint8_t>>, 2>{
        json_util::GetCheckedScoreArray(j, "team0"),
        json_util::GetCheckedScoreArray(j, "team1")
    });
}
/// @endcond

} // namespace digitalcurling
