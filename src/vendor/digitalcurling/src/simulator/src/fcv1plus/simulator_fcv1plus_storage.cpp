// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include "simulator_fcv1plus_storage.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include "fixed_types.hpp"
#include "simulator_fcv1plus.hpp"

namespace digitalcurling::simulators {
namespace {

std::int32_t ReadRaw(nlohmann::json const& json, char const* field)
{
    auto const& raw = json.at(field);
    if (!raw.is_number_integer()) {
        throw std::invalid_argument(
            std::string("SimulatorFCV1PlusStorage: ") + field + " must be an integer.");
    }
    if (raw.is_number_unsigned()) {
        auto const value = raw.get<std::uint64_t>();
        if (value >
            static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
        {
            throw std::out_of_range(
                std::string("SimulatorFCV1PlusStorage: ") + field +
                " is out of range.");
        }
        return static_cast<std::int32_t>(value);
    }
    auto const value = raw.get<std::int64_t>();
    if (value < std::numeric_limits<std::int32_t>::min() ||
        value > std::numeric_limits<std::int32_t>::max())
    {
        throw std::out_of_range(
            std::string("SimulatorFCV1PlusStorage: ") + field + " is out of range.");
    }
    return static_cast<std::int32_t>(value);
}

std::uint32_t ReadSchemaVersion(nlohmann::json const& json)
{
    // json.at("schema_version").get<std::uint32_t>() would let nlohmann
    // silently truncate a non-integer number (e.g. 1.5) or wrap an
    // out-of-range one, letting an unintended schema slip past this check.
    // Validate the JSON value's type and range explicitly first, mirroring
    // ReadRaw() above.
    auto const& raw = json.at("schema_version");
    if (!raw.is_number_integer()) {
        throw std::invalid_argument(
            "SimulatorFCV1PlusStorage: schema_version must be an integer.");
    }
    if (raw.is_number_unsigned()) {
        auto const value = raw.get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw std::out_of_range(
                "SimulatorFCV1PlusStorage: schema_version is out of range.");
        }
        return static_cast<std::uint32_t>(value);
    }
    auto const value = raw.get<std::int64_t>();
    if (value < 0 ||
        value > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()))
    {
        throw std::out_of_range(
            "SimulatorFCV1PlusStorage: schema_version is out of range.");
    }
    return static_cast<std::uint32_t>(value);
}

void ValidateRawStone(SimulatorFCV1PlusRawStone const& raw)
{
    using namespace fcv1plus_detail;
    if (!raw.enabled) {
        if (raw.position_x != 0 || raw.position_y != 0 || raw.angle != 0 ||
            raw.velocity_x != 0 || raw.velocity_y != 0 ||
            raw.angular_velocity != 0)
        {
            throw std::invalid_argument(
                "SimulatorFCV1PlusStorage: disabled stones must use canonical zero state.");
        }
        return;
    }

    StoneStateFixed stone;
    stone.enabled = true;
    stone.position = {
        Fixed::from_raw_value(raw.position_x),
        Fixed::from_raw_value(raw.position_y)};
    stone.angle = Fixed::from_raw_value(raw.angle);
    stone.velocity = {
        Fixed::from_raw_value(raw.velocity_x),
        Fixed::from_raw_value(raw.velocity_y)};
    stone.angular_velocity = Fixed::from_raw_value(raw.angular_velocity);
    ValidateRuntimeState(stone);
    if (stone.angle < -Fixed::pi() || stone.angle >= Fixed::pi()) {
        throw std::invalid_argument(
            "SimulatorFCV1PlusStorage: angle is not normalized to [-pi, pi).");
    }
}

} // namespace

SimulatorFCV1PlusStorage::SimulatorFCV1PlusStorage(
    SimulatorFCV1PlusFactory const& factory_value)
    : factory(factory_value)
    , stones()
{
}

nlohmann::json SimulatorFCV1PlusStorage::ToJson() const
{
    nlohmann::json result;
    to_json(result, *this);
    return result;
}

std::unique_ptr<ISimulator> SimulatorFCV1PlusStorage::CreateSimulator() const
{
    return std::make_unique<SimulatorFCV1Plus>(*this);
}

void to_json(nlohmann::json& json, SimulatorFCV1PlusRawStone const& value)
{
    ValidateRawStone(value);
    json = {
        {"enabled", value.enabled},
        {"position_x", value.position_x},
        {"position_y", value.position_y},
        {"angle", value.angle},
        {"velocity_x", value.velocity_x},
        {"velocity_y", value.velocity_y},
        {"angular_velocity", value.angular_velocity}
    };
}

void from_json(nlohmann::json const& json, SimulatorFCV1PlusRawStone& value)
{
    auto const& enabled = json.at("enabled");
    if (!enabled.is_boolean()) {
        throw std::invalid_argument(
            "SimulatorFCV1PlusStorage: enabled must be boolean.");
    }
    value.enabled = enabled.get<bool>();
    value.position_x = ReadRaw(json, "position_x");
    value.position_y = ReadRaw(json, "position_y");
    value.angle = ReadRaw(json, "angle");
    value.velocity_x = ReadRaw(json, "velocity_x");
    value.velocity_y = ReadRaw(json, "velocity_y");
    value.angular_velocity = ReadRaw(json, "angular_velocity");
    ValidateRawStone(value);
}

void to_json(nlohmann::json& json, SimulatorFCV1PlusStorage const& value)
{
    for (auto const& stone : value.stones) {
        ValidateRawStone(stone);
    }
    json["type"] = DIGITALCURLING_PLUGIN_NAME;
    json["schema_version"] = SimulatorFCV1PlusStorage::kSchemaVersion;
    json["factory"] = value.factory;
    json["stones"] = value.stones;
}

void from_json(nlohmann::json const& json, SimulatorFCV1PlusStorage& value)
{
    auto const version = ReadSchemaVersion(json);
    if (version != SimulatorFCV1PlusStorage::kSchemaVersion) {
        throw std::invalid_argument(
            "SimulatorFCV1PlusStorage: unsupported schema version.");
    }

    json.at("factory").get_to(value.factory);
    auto const& stones_json = json.at("stones");
    if (!stones_json.is_array() ||
        stones_json.size() != SimulatorFCV1PlusStorage::RawStones{}.size())
    {
        throw std::invalid_argument(
            "SimulatorFCV1PlusStorage: stones must contain exactly 16 entries.");
    }
    value.stones = stones_json.get<SimulatorFCV1PlusStorage::RawStones>();
    // A "collisions" key written by earlier versions is accepted and ignored.
}

} // namespace digitalcurling::simulators
