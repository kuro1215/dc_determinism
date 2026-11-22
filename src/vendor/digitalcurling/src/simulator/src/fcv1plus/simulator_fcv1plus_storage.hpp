// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <nlohmann/json.hpp>
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#include "simulator_fcv1plus_factory.hpp"

namespace digitalcurling::simulators {

struct SimulatorFCV1PlusRawStone {
    bool enabled = false;
    std::int32_t position_x = 0;
    std::int32_t position_y = 0;
    std::int32_t angle = 0;
    std::int32_t velocity_x = 0;
    std::int32_t velocity_y = 0;
    std::int32_t angular_velocity = 0;

    friend bool operator==(
        SimulatorFCV1PlusRawStone const& lhs,
        SimulatorFCV1PlusRawStone const& rhs) noexcept
    {
        return lhs.enabled == rhs.enabled &&
               lhs.position_x == rhs.position_x &&
               lhs.position_y == rhs.position_y &&
               lhs.angle == rhs.angle &&
               lhs.velocity_x == rhs.velocity_x &&
               lhs.velocity_y == rhs.velocity_y &&
               lhs.angular_velocity == rhs.angular_velocity;
    }
};

class SimulatorFCV1PlusStorage : public ISimulatorStorage {
public:
    static constexpr std::uint32_t kSchemaVersion = 1;
    using RawStones =
        std::array<SimulatorFCV1PlusRawStone, StoneCoordinate::kStoneMax>;

    SimulatorFCV1PlusStorage() = default;
    SimulatorFCV1PlusStorage(SimulatorFCV1PlusStorage const&) = default;
    SimulatorFCV1PlusStorage& operator=(SimulatorFCV1PlusStorage const&) = default;
    explicit SimulatorFCV1PlusStorage(SimulatorFCV1PlusFactory const& factory_value);
    ~SimulatorFCV1PlusStorage() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    nlohmann::json ToJson() const override;
    std::unique_ptr<ISimulator> CreateSimulator() const override;

    SimulatorFCV1PlusFactory factory;
    RawStones stones;
};

void to_json(nlohmann::json&, SimulatorFCV1PlusRawStone const&);
void from_json(nlohmann::json const&, SimulatorFCV1PlusRawStone&);
void to_json(nlohmann::json&, SimulatorFCV1PlusStorage const&);
void from_json(nlohmann::json const&, SimulatorFCV1PlusStorage&);

} // namespace digitalcurling::simulators
