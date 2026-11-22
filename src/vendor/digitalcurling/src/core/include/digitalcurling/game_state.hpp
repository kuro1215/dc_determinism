// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameState

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/game_scores.hpp"
#include "digitalcurling/game_setting.hpp"
#include "digitalcurling/game_result.hpp"

#include "digitalcurling/game_rule.hpp"

#include "digitalcurling/stone.hpp"
#include "digitalcurling/stone_coordinate.hpp"
#include "digitalcurling/team.hpp"
#include "digitalcurling/vector2.hpp"

namespace digitalcurling {


/// @brief Game state
///
/// @note The state is validated and built by the constructor, and is read through accessors afterwards.
struct GameState {

    /// @brief Constructor
    /// @param[in] reserved_stone_numbers Stone number of each team excluded from validation as a mixed doubles pre-positioned stone
    /// The core library currently has no convention for the stone numbers used by pre-positioned stones, so the caller specifies the numbers actually used.
    /// If simulators establish such a convention, this mechanism may be replaced by stricter rule-based validation.
    GameState(std::uint8_t max_end,
              std::uint8_t end = 0,
              std::uint8_t shot = 0,
              Team hammer = Team::k1,
              std::optional<StoneCoordinate> stones = std::nullopt,
              std::optional<GameScores> scores = std::nullopt,
              std::optional<TeamValue<std::chrono::milliseconds>> thinking_time_remaining = std::nullopt,
              std::optional<GameResult> game_result = std::nullopt,
              TeamValue<std::optional<std::uint8_t>> reserved_stone_numbers = {})
        : end_(end)
        , shot_(shot)
        , hammer_(hammer)
        , stones_()
        , reserved_stone_numbers_(reserved_stone_numbers)
        , scores_(max_end)
        , thinking_time_remaining_()
        , game_result_(std::move(game_result))
    {
        if (end >= GameRule::kEndLimit)
            throw std::invalid_argument("invalid end data: end must be less than 255.");
        if (shot > StoneCoordinate::kStoneMax)
            throw std::invalid_argument("invalid shot data: shot must be between 0 and 16.");

        for (Team team : { Team::k0, Team::k1 }) {
            if (reserved_stone_numbers[team].has_value()
                && reserved_stone_numbers[team].value() >= StoneCoordinate::kStoneMax / 2)
                throw std::invalid_argument("invalid reserved stone number: stone number must be between 0 and 7.");
        }

        // stones
        if (stones.has_value()) {
            const bool is_odd_shot = shot % 2 == 1;
            const int start = shot / 2, end = StoneCoordinate::kStoneMax / 2;

            for (Team team : { Team::k0, Team::k1 }) {
                for (int i = start + (is_odd_shot && hammer != team ? 1 : 0); i < end; i++) {
                    if ((*stones)[StoneIndex(team, i)].has_value()
                        && (!reserved_stone_numbers[team].has_value() || reserved_stone_numbers[team].value() != i))
                        throw std::invalid_argument("invalid stone data: shot is less than the number of stones on the board.");
                }
            }
            this->stones_ = std::move(stones.value());
        }

        // scores
        if (scores.has_value()) {
            if (max_end != scores->GetEndCount())
                throw std::invalid_argument("invalid scores data: max_end does not match the size of scores.");

            // boundary check for scores data
            // All points expect the boudary have been checked in GameScores constructor.
            int check_separator = std::min(end - 1 + (game_result.has_value() ? 1 : 0), static_cast<int>(max_end));
            if (check_separator > 0 && (!(*scores)[Team::k0][check_separator].has_value() || !(*scores)[Team::k1][check_separator].has_value()))
                throw std::invalid_argument("invalid scores data: scores for completed ends must be determined.");
            if (check_separator < max_end && ((*scores)[Team::k0][check_separator + 1].has_value() || (*scores)[Team::k1][check_separator + 1].has_value()))
                throw std::invalid_argument("invalid scores data: scores for incomplete ends must be undetermined.");

            this->scores_ = std::move(scores.value());
        }

        if (thinking_time_remaining.has_value())
            this->thinking_time_remaining_ = std::move(thinking_time_remaining.value());
    }

    /// @brief Copy constructor
    GameState(GameState const&) = default;
    /// @brief Move constructor
    GameState(GameState &&) = default;

    /// @brief Copy assignment operator
    GameState& operator = (GameState const&) = default;
    /// @brief Move assignment operator
    GameState& operator = (GameState &&) = default;

    /// @brief Gets the current end
    /// @returns Current end
    std::uint8_t GetEnd() const { return end_; }

    /// @brief Gets the current shot number
    /// @returns Current shot number (0-16)
    std::uint8_t GetShot() const { return shot_; }

    /// @brief Gets the hammer team in the current end
    /// @returns Hammer team in the current end
    Team GetHammer() const { return hammer_; }

    /// @brief Gets the position and angle of each team's stones
    /// @returns Position and angle of each team's stones
    StoneCoordinate const& GetStones() const { return stones_; }

    /// @brief Gets the stone number of each team excluded from validation as a pre-positioned stone
    /// @returns Stone number of each team excluded from validation as a pre-positioned stone
    TeamValue<std::optional<std::uint8_t>> const& GetReservedStoneNumbers() const { return reserved_stone_numbers_; }

    /// @brief Gets the scores for each end
    /// @returns Scores for each end
    GameScores const& GetScores() const { return scores_; }

    /// @brief Gets each team's remaining thinking time
    /// @returns Each team's remaining thinking time
    TeamValue<std::chrono::milliseconds> const& GetThinkingTimeRemaining() const { return thinking_time_remaining_; }

    /// @brief Gets the game result
    /// @returns Game result
    /// @note A value of `std::nullopt` means the game is in progress.
    std::optional<GameResult> const& GetGameResult() const { return game_result_; }

    /// @brief Gets the team to act next
    /// @returns Team to act next (`std::nullopt` if the game is already over)
    std::optional<Team> GetNextTeam() const {
        if (IsGameOver()) return std::nullopt;

        Team next = shot_ % 2 == 0 ? GetOpponentTeam(hammer_) : hammer_;
        return next;
    }

    /// @brief Checks whether the game has ended
    /// @return `true` if the game has ended
    bool IsGameOver() const { return game_result_.has_value(); }

private:
    std::uint8_t end_;
    std::uint8_t shot_;
    Team hammer_;
    StoneCoordinate stones_;
    TeamValue<std::optional<std::uint8_t>> reserved_stone_numbers_;
    GameScores scores_;
    TeamValue<std::chrono::milliseconds> thinking_time_remaining_;
    std::optional<GameResult> game_result_;

};


/// @cond Doxygen_Suppress
// json
namespace {

inline TeamValue<std::optional<std::uint8_t>> get_reserved_stone_numbers(const nlohmann::json& j) {
    if (!j.contains("reserved_stone_numbers")) {
        return {};
    }

    auto get_stone_number = [](const nlohmann::json& value) -> std::optional<std::uint8_t> {
        if (value.is_null()) {
            return std::nullopt;
        }

        return json_util::GetCheckedInteger<std::uint8_t>(value, "reserved stone number");
    };

    auto const& reserved_stone_numbers = j.at("reserved_stone_numbers");
    return TeamValue<std::optional<std::uint8_t>>(
        get_stone_number(reserved_stone_numbers.at("team0")),
        get_stone_number(reserved_stone_numbers.at("team1"))
    );
}

} // unnamed namespace

inline void to_json(nlohmann::json& j, const GameState& v) {
    j = nlohmann::json{
        {"end", v.GetEnd()},
        {"shot", v.GetShot()},
        {"hammer", v.GetHammer()},
        {"stones", v.GetStones()},
        {"reserved_stone_numbers", v.GetReservedStoneNumbers()},
        {"scores", v.GetScores()},
        {"thinking_time_remaining", v.GetThinkingTimeRemaining()},
        {"game_result", v.GetGameResult()}
    };
}
inline void from_json(const nlohmann::json& j, GameState& v) {
    auto end = json_util::GetChecked<std::uint8_t>(j, "end");
    auto shot = json_util::GetChecked<std::uint8_t>(j, "shot");
    auto hammer = j.at("hammer").get<Team>();
    auto stones = j.at("stones").get<StoneCoordinate>();
    auto scores = j.at("scores").get<GameScores>();
    auto thinking_time_remaining = j.at("thinking_time_remaining").get<TeamValue<std::chrono::milliseconds>>();
    auto game_result = j.at("game_result").get<std::optional<GameResult>>();
    auto reserved_stone_numbers = get_reserved_stone_numbers(j);

    v = GameState(
        scores.GetEndCount(),
        end,
        shot,
        hammer,
        std::move(stones),
        std::move(scores),
        std::move(thinking_time_remaining),
        std::move(game_result),
        std::move(reserved_stone_numbers)
    );
}

/// @endcond

} // namespace digitalcurling
