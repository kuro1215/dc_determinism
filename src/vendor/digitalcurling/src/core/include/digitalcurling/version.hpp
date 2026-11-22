// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines the library version constants
/// @details The same values are available as the preprocessor macros
///          `DIGITALCURLING_VERSION_MAJOR`, `DIGITALCURLING_VERSION_MINOR`,
///          `DIGITALCURLING_VERSION_PATCH` (integers) and
///          `DIGITALCURLING_VERSION_STRING` (string literal, e.g. `"4.1.0"`).

#pragma once

#include <string_view>

#include "digitalcurling/detail/version_info.hpp"

namespace digitalcurling {

/// @brief Major version of the library
inline constexpr int kLibraryVersionMajor = DIGITALCURLING_VERSION_MAJOR;

/// @brief Minor version of the library
inline constexpr int kLibraryVersionMinor = DIGITALCURLING_VERSION_MINOR;

/// @brief Patch version of the library
inline constexpr int kLibraryVersionPatch = DIGITALCURLING_VERSION_PATCH;

/// @brief Version string of the library in the format `major.minor.patch` (e.g. `"4.1.0"`)
inline constexpr std::string_view kLibraryVersionString = DIGITALCURLING_VERSION_STRING;

} // namespace digitalcurling
