// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"
#include "simulator_fcv1_factory.hpp"
#include "simulator_fcv1.hpp"

namespace digitalcurling::simulators {

nlohmann::json SimulatorFCV1Factory::ToJson() const {
    nlohmann::json j;
    to_json(j, *this);
    return j;
}

std::unique_ptr<ISimulator> SimulatorFCV1Factory::CreateSimulator() const {
    return std::make_unique<SimulatorFCV1>(*this);
}

std::unique_ptr<ISimulatorFactory> SimulatorFCV1Factory::Clone() const {
    return std::make_unique<SimulatorFCV1Factory>(*this);
}

// json
void to_json(nlohmann::json & j, SimulatorFCV1Factory const&) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["seconds_per_frame"] = SimulatorFCV1::kSecondsPerFrame;
}
void from_json(nlohmann::json const& j, SimulatorFCV1Factory &) {
    // The time step is fixed; seconds_per_frame is accepted for compatibility and ignored.
    (void)j.get_ref<nlohmann::json::object_t const&>();
}

} // namespace digitalcurling::simulators
