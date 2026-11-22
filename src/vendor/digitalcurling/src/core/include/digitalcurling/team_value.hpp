// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Team and TeamValue

#pragma once

#include <array>
#include <cassert>
#include <nlohmann/json.hpp>
#include "digitalcurling/team.hpp"

namespace digitalcurling {


/// @brief Values for each team
/// @tparam TValue Type of the value
template <typename TValue>
struct TeamValue {

private:
    std::array<TValue, 2> values_;

protected:
    TValue& GetMutable(Team team) {
        return values_[team == Team::k0 ? 0 : 1];
    }

public:
    /// @brief Constructor
    TeamValue() : values_{} {}

    /// @brief Constructor
    /// @param[in] value Initial value (the same for team0 and team1)
    TeamValue(TValue value) : values_{value, value} {}

    /// @brief Constructor
    /// @param[in] team0 Value for team 0
    /// @param[in] team1 Value for team 1
    TeamValue(TValue team0, TValue team1) : values_{team0, team1} {}

    /// @brief Copy constructor
    TeamValue(TeamValue<TValue> const&) = default;
    /// @brief Move constructor
    TeamValue(TeamValue<TValue>&&) = default;

    /// @brief Copy assignment operator
    TeamValue<TValue>& operator=(TeamValue<TValue> const&) = default;
    /// @brief Move assignment operator
    TeamValue<TValue>& operator=(TeamValue<TValue>&&) = default;

    /// @brief Gets a team's value
    /// @param[in] team Team
    TValue const& operator[](Team team) const {
        return values_[team == Team::k0 ? 0 : 1];
    }

    /// @brief Gets a team's value
    ///
    /// The first value corresponds to team 0 and the second to team 1.
    std::array<TValue, 2> ToArray() {
        return values_;
    }

    /// @cond Doxygen_Suppress
    // json
    template<typename BasicJsonType, nlohmann::detail::enable_if_t<nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
    friend void to_json(BasicJsonType& j, TeamValue const& value) {
        j["team0"] = value.values_[0];
        j["team1"] = value.values_[1];
    }

    template<typename BasicJsonType, nlohmann::detail::enable_if_t<nlohmann::detail::is_basic_json<BasicJsonType>::value, int> = 0>
    friend void from_json(BasicJsonType const& j, TeamValue& value) {
        j.at("team0").get_to(value.values_[0]);
        j.at("team1").get_to(value.values_[1]);
    }
    /// @endcond
};

} // namespace digitalcurling
