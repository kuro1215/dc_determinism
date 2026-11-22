// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "player_nd2.hpp"
#include "digitalcurling/plugins/player_plugin_export.hpp"

DIGITALCURLING_EXPORT_PLAYER_PLUGIN(
    digitalcurling::players::PlayerND2Factory,
    digitalcurling::players::PlayerND2Storage,
    digitalcurling::players::PlayerND2
)
