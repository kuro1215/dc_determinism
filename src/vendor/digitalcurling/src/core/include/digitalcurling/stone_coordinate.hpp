// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines StoneCoordinate

#pragma once

#include <array>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>
#include <algorithm>
#include <iterator>
#include "digitalcurling/common.hpp"
#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/stone.hpp"
#include "digitalcurling/stone_index.hpp"

namespace digitalcurling {


/// @brief Coordinates of stones on the sheet
///
/// A stone absent from the board is represented by `std::nullopt`.
///
/// Ex.)
/// ```cpp
/// StoneCoordinate stones;
/// stones[Team::k0][0]; // Team 0's first stone
/// stones[Team::k1][7]; // Team 1's eighth stone
/// ```
class StoneCoordinate : public TeamValue<std::array<std::optional<Stone>, 8>> {
public:
    /// @brief Maximum number of stones
    static constexpr int kStoneMax = 16;

    /// @brief Constructor
    StoneCoordinate() : TeamValue<std::array<std::optional<Stone>, 8>>() {
        for (auto& stone : GetMutable(Team::k0)) stone = std::nullopt;
        for (auto& stone : GetMutable(Team::k1)) stone = std::nullopt;
    }

    /// @brief Constructor
    /// @param[in] stones Stone information
    StoneCoordinate(std::array<std::array<std::optional<Stone>, 8>, 2> stones)
        : TeamValue<std::array<std::optional<Stone>, 8>>(std::move(stones[0]), std::move(stones[1])) {}

    /// @brief Constructor
    /// @param[in] stones Stone information
    StoneCoordinate(std::array<std::optional<Stone>, kStoneMax> stones)
        : TeamValue<std::array<std::optional<Stone>, 8>>()
    {
        std::move(stones.begin(), stones.begin() + 8, GetMutable(Team::k0).begin());
        std::move(stones.begin() + 8, stones.end(), GetMutable(Team::k1).begin());
    }

    /// @brief Gets stone information
    /// @param[in] index Stone index
    /// @returns Stone information
    std::optional<Stone> const& operator[](StoneIndex index) const {
        return GetStone(index);
    }
    using TeamValue<std::array<std::optional<Stone>, 8>>::operator[];

    /// @brief Gets stone information
    /// @param[in] index Stone index
    /// @returns Stone information
    std::optional<Stone> const& GetStone(StoneIndex index) const {
        return GetTeamStones(index.GetTeam())[index.GetStoneNumber()];
    }

    /// @brief Gets information for all stones of a team
    /// @param[in] team Team
    /// @returns Information for all stones of a team
    std::array<std::optional<Stone>, 8> const& GetTeamStones(Team team) const {
        return this->operator[](team);
    }

    /// @brief Gets information for all stones
    /// @returns Information for all stones
    std::array<std::optional<Stone>, kStoneMax> GetAllStones() const {
        std::array<std::optional<Stone>, kStoneMax> result;
        auto const& team0 = GetTeamStones(Team::k0);
        auto const& team1 = GetTeamStones(Team::k1);
        auto dest = std::copy(team0.begin(), team0.end(), result.begin());
        std::copy(team1.begin(), team1.end(), dest);
        return result;
    }

    /// @brief Gets the indices of all valid stones on the board
    /// @returns Indices of valid stones
    std::vector<StoneIndex> GetActiveStones() const {
        std::vector<StoneIndex> active_stones;
        for (Team team : { Team::k0, Team::k1 }) {
            for (uint8_t i = 0; i < 8; i++) {
                auto idx = StoneIndex(team, i);
                if (GetStone(idx).has_value()) active_stones.push_back(idx);
            }
        }
        return active_stones;
    }

    /// @brief Gets all stones on the board sorted by distance from the center of the house
    /// @returns Pairs of valid stone indices and distances, ordered by distance from the center of the house
    std::vector<StoneIndex> GetSortedIndex() const {
        auto stones = GetActiveStones();
        std::sort(stones.begin(), stones.end(), [this](StoneIndex const& a, StoneIndex const& b) {
            return GetStone(a).value().GetDistanceFromTee() < GetStone(b).value().GetDistanceFromTee();
        });
        return stones;
    }

    /// @brief Gets all stones on the board sorted by distance from the center of the house
    /// @returns Pairs of valid stone information and distances, ordered by distance from the center of the house
    std::vector<Stone> GetSortedStones() const {
        auto const idxs = GetSortedIndex();
        std::vector<Stone> sorted;
        std::transform(idxs.begin(), idxs.end(), std::back_inserter(sorted), [this](StoneIndex const& i) {
            return GetStone(i).value();
        });
        return sorted;
    }
};

} // namespace digitalcurling
