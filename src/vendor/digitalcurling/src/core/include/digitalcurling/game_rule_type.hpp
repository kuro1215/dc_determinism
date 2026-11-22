// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines GameRuleType

#pragma once

#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

namespace digitalcurling {

/// @brief Type of game rule
enum class GameRuleType {
    /// @brief Standard four-player rules
    kStandard,
    /// @brief Mixed rules
    kMixed,
    /// @brief Mixed doubles rules
    kMixedDoubles
};

/// @cond Doxygen_Suppress
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(GameRuleType, {
    { GameRuleType::kStandard, "standard" },
    { GameRuleType::kMixed, "mixed" },
    { GameRuleType::kMixedDoubles, "mixed_doubles" },
});
/// @endcond

} // namespace digitalcurling
