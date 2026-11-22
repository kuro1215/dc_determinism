// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <memory>
#include "digitalcurling/plugins/plugin_api_v2.h"

namespace digitalcurling::plugins::detail {

// Shared ownership lets host proxy handles keep a dynamically loaded module
// resident until their remote v2 destructor has returned.
using ModulePtr = std::shared_ptr<void>;

struct NativePlugin {
    ModulePtr handle;
    DigitalCurling_GetPluginApiV2Fn get_plugin_api_v2 = nullptr;
};

NativePlugin LoadNativePlugin(const std::filesystem::path& path);

} // namespace digitalcurling::plugins::detail
