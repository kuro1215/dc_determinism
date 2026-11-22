// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines IPlayer

#pragma once

#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

namespace digitalcurling::players {

/// @brief Player's gender
enum class Gender : int8_t {
    /// @brief Male
    kMale,
    /// @brief Female
    kFemale,
    /// @brief Unknown gender
    kUnknown = -1,
};

/// @cond Doxygen_Suppress
DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(Gender, {
    {Gender::kMale, "male"},
    {Gender::kFemale, "female"},
    {Gender::kUnknown, "unknown"},
})
/// @endcond

} // namespace digitalcurling::players
