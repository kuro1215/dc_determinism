// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines plugin-related errors

#pragma once

#include <stdexcept>
#include <string>
#include "digitalcurling/plugins/data_object.h"

namespace digitalcurling::plugins {


/// @brief Exception class representing a plugin operation error
class plugin_error : public std::runtime_error {
public:
    /// @brief Constructor
    /// @param code Error code identifying the error type
    /// @param message Detailed error message
    explicit plugin_error(DigitalCurling_ErrorCode code, const std::string& message)
        : std::runtime_error(message), error_code_(code) {}

    /// @brief Gets the error code
    /// @return Error code
    DigitalCurling_ErrorCode code() const noexcept
    {
        return error_code_;
    }

private:
    DigitalCurling_ErrorCode error_code_;
};

} // namespace digitalcurling::plugins
