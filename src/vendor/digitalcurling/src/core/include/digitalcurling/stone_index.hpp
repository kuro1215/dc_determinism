// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines StoneIndex

#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/team.hpp"

namespace digitalcurling {


/// @brief Stone index
struct StoneIndex {

    /// @brief Constructor
    StoneIndex() : index_(0) {}

    /// @brief Constructor
    /// @param index Stone index (0-15)
    StoneIndex(int index) : index_(index) {
        if (index < 0 || index >= 16)
            throw std::invalid_argument("index must be between 0 and 15");
    }

    /// @brief Constructor
    /// @param[in] team Team
    /// @param[in] stone Stone number
    StoneIndex(Team team, std::uint8_t stone) {
        if (stone >= 8)
            throw std::invalid_argument("stone must be less than 8");

        index_ = (team == Team::k0 ? 0 : 8) + stone;
    }

    /// @brief Gets the team
    /// @return Team
    Team GetTeam() const { return index_ < 8 ? Team::k0 : Team::k1; }
    /// @brief Gets the stone number
    /// @return Stone number
    std::uint8_t GetStoneNumber() const { return index_ % 8; }
    /// @brief Gets the stone index
    /// @return Stone index (0-15)
    std::uint8_t GetIndex() const { return index_; }

    /// @brief Converts to the stone index
    operator std::uint8_t() const { return GetIndex(); }
    /// @brief Converts to a pair of team and stone number
    operator std::pair<Team, std::uint8_t>() const {
        return { GetTeam(), GetStoneNumber() };
    }

private:
    std::uint8_t index_;

};


/// @cond Doxygen_Suppress
// json

inline void to_json(nlohmann::json& j, const StoneIndex& v) {
    j = nlohmann::json{
        {"team", v.GetTeam()},
        {"stone", v.GetStoneNumber()}
    };
}
inline void from_json(const nlohmann::json& j, StoneIndex& v) {
    auto team = j.at("team").get<Team>();
    auto stone = json_util::GetChecked<std::uint8_t>(j, "stone");
    v = StoneIndex(team, stone);
}

/// @endcond

} // namespace digitalcurling
