// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Team and TeamValue

#pragma once

#include <cstdint>
#include <cassert>
#include <string>
#include <type_traits>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

namespace digitalcurling {


/// @name Teams
/// @{

/// @brief Team
enum class Team : bool {
    /// @brief Team 0 (the team without hammer in the first end)
    k0 = false,
    /// @brief Team 1 (the team with hammer in the first end)
    k1 = true
};

/// @brief Gets the opposing team
/// @param[in] team Team
/// @return The team opposing the given team
inline Team GetOpponentTeam(Team team)
{
    return team == Team::k0 ? Team::k1 : Team::k0;
}

/// @brief Converts a team to a string
/// @param[in] team Team
/// @return Team string
inline std::string ToString(Team team)
{
    return team == Team::k0 ? "team0" : "team1";
}

/// @brief Converts an enum class to `std::underlying_type_t`
/// @param[in] value enum class
/// @return `static_cast<std::underlying_type_t<T>>(value)`
template <class T>
inline std::underlying_type_t<T> ToUType(T value)
{
    return static_cast<std::underlying_type_t<T>>(value);
}

/// @}


/// @cond Doxygen_Suppress
// json
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(Team, {
    {Team::k0, "team0"},
    {Team::k1, "team1"}
})
/// @endcond

} // namespace digitalcurling

#include "digitalcurling/team_value.hpp"
