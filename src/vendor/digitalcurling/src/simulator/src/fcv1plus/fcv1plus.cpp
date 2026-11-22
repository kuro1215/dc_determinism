// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "simulator_fcv1plus.hpp"
#include "digitalcurling/plugins/simulator_plugin_export.hpp"

DIGITALCURLING_EXPORT_INVERTIBLE_SIMULATOR_PLUGIN(
    digitalcurling::simulators::SimulatorFCV1PlusFactory,
    digitalcurling::simulators::SimulatorFCV1PlusStorage,
    digitalcurling::simulators::SimulatorFCV1Plus,
    &digitalcurling::simulators::SimulatorFCV1Plus::CalculateShot
)
