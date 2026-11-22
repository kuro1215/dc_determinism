// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameSetting

#pragma once

#include <cstdint>
#include <chrono>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "digitalcurling/team.hpp"

namespace digitalcurling {


/// @brief Game settings
struct GameSetting {

    /// @brief Width of the ice sheet
    float sheet_width = 4.75f;

    /// @brief Highest end number that may be played, including extra ends
    ///
    /// A match still tied after this end is expected to be treated as a draw by its host.
    std::uint8_t end_limit = 255;

    /// @brief Thinking time for each team across all ends except extra ends
    /// @sa extra_end_thinking_time Thinking time for extra ends
    TeamValue<std::chrono::milliseconds> thinking_time;

    /// @brief Thinking time for extra ends
    ///
    /// The remaining thinking time is reset to this value at the start of each extra end.
    ///
    /// @sa thinking_time Thinking time for non-extra ends
    TeamValue<std::chrono::milliseconds> extra_end_thinking_time;
};


/// @cond Doxygen_Suppress
// json
inline void to_json(nlohmann::json& j, GameSetting const& v) {
    j["sheet_width"] = v.sheet_width;
    j["end_limit"] = v.end_limit;
    j["thinking_time"] = v.thinking_time;
    j["extra_end_thinking_time"] = v.extra_end_thinking_time;
}
inline void from_json(nlohmann::json const& j, GameSetting& v) {
    v.sheet_width = json_util::GetCheckedFloatAt(j, "sheet_width");
    v.end_limit = j.contains("end_limit") ? json_util::GetChecked<std::uint8_t>(j, "end_limit") : std::uint8_t(255);
    j.at("thinking_time").get_to(v.thinking_time);
    j.at("extra_end_thinking_time").get_to(v.extra_end_thinking_time);
}
/// @endcond

} // namespace digitalcurling
