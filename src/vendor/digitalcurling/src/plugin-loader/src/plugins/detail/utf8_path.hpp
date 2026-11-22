// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <string>
#include "digitalcurling_plugin_loader_export.h"

namespace digitalcurling::plugins::detail {

DIGITALCURLING_PLUGIN_LOADER_NO_EXPORT bool IsValidUtf8(const std::string& value);
DIGITALCURLING_PLUGIN_LOADER_NO_EXPORT std::string PathToUtf8(const std::filesystem::path& path);

} // namespace digitalcurling::plugins::detail
