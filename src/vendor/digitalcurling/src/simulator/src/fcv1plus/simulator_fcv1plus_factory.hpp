// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "fcv1plus.hpp"

namespace digitalcurling::simulators {

class SimulatorFCV1PlusFactory : public ISimulatorFactory {
public:
    SimulatorFCV1PlusFactory() = default;
    SimulatorFCV1PlusFactory(SimulatorFCV1PlusFactory const&) = default;
    SimulatorFCV1PlusFactory& operator=(SimulatorFCV1PlusFactory const&) = default;
    ~SimulatorFCV1PlusFactory() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    nlohmann::json ToJson() const override;
    std::unique_ptr<ISimulator> CreateSimulator() const override;
    std::unique_ptr<ISimulatorFactory> Clone() const override;
};

void to_json(nlohmann::json&, SimulatorFCV1PlusFactory const&);
void from_json(nlohmann::json const&, SimulatorFCV1PlusFactory&);

} // namespace digitalcurling::simulators
