// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameResult

#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include "digitalcurling/team.hpp"

namespace digitalcurling {


/// @brief Game result
struct GameResult {
    /// @brief Reason for the game outcome
    enum class Reason : std::uint8_t {
        /// @brief Outcome decided by score
        kScore,
        /// @brief Outcome decided by concession
        kConcede,
        /// @brief Outcome decided by timeout
        kTimeLimit,
        /// @brief Draw
        kDraw,
        /// @brief Outcome decided by a team's disconnection
        kDisconnection
    };

    /// @brief Winning team
    ///
    /// `Team::k0` in case of a draw
    Team winner = Team::k0;

    /// @brief Reason for the game outcome
    Reason reason = Reason::kDraw;
};


/// @cond Doxygen_Suppress
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(GameResult::Reason, {
    {GameResult::Reason::kScore, "score"},
    {GameResult::Reason::kConcede, "concede"},
    {GameResult::Reason::kTimeLimit, "time_limit"},
    {GameResult::Reason::kDraw, "draw"},
    {GameResult::Reason::kDisconnection, "disconnection"},
})

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GameResult, winner, reason)
/// @endcond

} // namespace digitalcurling
