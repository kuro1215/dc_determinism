// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines Concede

#pragma once

#include <string_view>

namespace digitalcurling::moves {


/// @brief Move: concede
/// @sa Move
struct Concede {
    /// @brief Type name identifying the `Move`
    static constexpr std::string_view kTypeName = "concede";
};

} // namespace digitalcurling::moves
