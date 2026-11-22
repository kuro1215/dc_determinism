// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "player_normal_dist.hpp"

namespace digitalcurling::players {

namespace {

PlayerNormalDistFactory const& Validated(PlayerNormalDistFactory const& factory)
{
    factory.Validate();
    return factory;
}

/// @brief Reads a whole state from a stream into a temporary
/// @throws std::invalid_argument If the data is not read completely or contains trailing characters
template <typename T>
T ReadState(std::string const& data, char const* name)
{
    std::istringstream stream(data);
    T state;
    stream >> state;
    if (!stream || !(stream >> std::ws).eof()) {
        throw std::invalid_argument(std::string("normal_dist: invalid ") + name);
    }
    return state;
}

void ValidateDistribution(std::normal_distribution<float> const& dist, char const* name)
{
    if (!std::isfinite(dist.mean()) || !std::isfinite(dist.stddev()) || !(dist.stddev() > 0.f)) {
        throw std::invalid_argument(std::string("normal_dist: invalid ") + name);
    }
}

} // namespace

PlayerNormalDist::PlayerNormalDist(PlayerNormalDistFactory const& factory)
    : factory_(Validated(factory))
    , engine_(std::nullopt)
    , speed_dist_(0.f, factory.stddev_speed)
    , angle_dist_(0.f, factory.stddev_angle)
{
    if (!factory_.seed.has_value()) {
        factory_.seed = std::random_device()();
    }
    engine_.emplace(factory_.seed.value());
}

PlayerNormalDist::PlayerNormalDist(PlayerNormalDistStorage const& storage)
    : factory_(Validated(storage.factory))
    , engine_(std::mt19937())
    , speed_dist_()
    , angle_dist_()
{
    LoadEngine(storage.engine_data, storage.speed_dist_data, storage.angle_dist_data);
}

moves::Shot PlayerNormalDist::Play(moves::Shot const& shot)
{
    float const speed = std::min(shot.translational_velocity, factory_.max_speed) + speed_dist_(engine_.value());
    float const angle = shot.release_angle + angle_dist_(engine_.value());

    return moves::Shot { speed, shot.angular_velocity, angle };
}

std::unique_ptr<IPlayerStorage> PlayerNormalDist::CreateStorage() const
{
    auto storage = std::make_unique<PlayerNormalDistStorage>();
    Save(*storage);
    return storage;
}

void PlayerNormalDist::Save(IPlayerStorage & storage) const
{
    auto & s = dynamic_cast<PlayerNormalDistStorage &>(storage);
    s.factory = factory_;

    std::ostringstream s_engine;
    s_engine << *engine_;
    s.engine_data = s_engine.str();

    std::ostringstream s_speed_dist;
    s_speed_dist << speed_dist_;
    s.speed_dist_data = s_speed_dist.str();

    std::ostringstream s_angle_dist;
    s_angle_dist << angle_dist_;
    s.angle_dist_data = s_angle_dist.str();
}

void PlayerNormalDist::Load(IPlayerStorage const& storage)
{
    auto const& s = dynamic_cast<PlayerNormalDistStorage const&>(storage);
    s.factory.Validate();
    LoadEngine(s.engine_data, s.speed_dist_data, s.angle_dist_data);
    factory_ = s.factory;
}

void PlayerNormalDist::LoadEngine(
    std::string const& engine_data,
    std::string const& speed_dist_data,
    std::string const& angle_dist_data)
{
    // Parse everything into temporaries first so that a failure leaves this player unchanged.
    auto engine = ReadState<std::mt19937>(engine_data, "engine_data");
    auto speed_dist = ReadState<std::normal_distribution<float>>(speed_dist_data, "speed_dist_data");
    auto angle_dist = ReadState<std::normal_distribution<float>>(angle_dist_data, "angle_dist_data");
    ValidateDistribution(speed_dist, "speed_dist_data");
    ValidateDistribution(angle_dist, "angle_dist_data");

    *engine_ = std::move(engine);
    speed_dist_ = std::move(speed_dist);
    angle_dist_ = std::move(angle_dist);
}

} // namespace digitalcurling::players
