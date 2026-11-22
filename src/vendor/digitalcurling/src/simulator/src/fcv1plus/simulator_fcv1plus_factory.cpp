// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "simulator_fcv1plus_factory.hpp"

#include <memory>
#include "simulator_fcv1plus.hpp"

namespace digitalcurling::simulators {

nlohmann::json SimulatorFCV1PlusFactory::ToJson() const
{
    nlohmann::json result;
    to_json(result, *this);
    return result;
}

std::unique_ptr<ISimulator> SimulatorFCV1PlusFactory::CreateSimulator() const
{
    return std::make_unique<SimulatorFCV1Plus>(*this);
}

std::unique_ptr<ISimulatorFactory> SimulatorFCV1PlusFactory::Clone() const
{
    return std::make_unique<SimulatorFCV1PlusFactory>(*this);
}

void to_json(nlohmann::json& json, SimulatorFCV1PlusFactory const&)
{
    json["type"] = DIGITALCURLING_PLUGIN_NAME;
    json["seconds_per_frame"] = SimulatorFCV1Plus::kSecondsPerFrame;
}

void from_json(nlohmann::json const& json, SimulatorFCV1PlusFactory&)
{
    (void)json.get_ref<nlohmann::json::object_t const&>();
}

} // namespace digitalcurling::simulators
