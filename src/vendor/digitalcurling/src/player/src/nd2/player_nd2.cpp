// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <algorithm>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <fpm/fixed.hpp>
#include <fpm/math.hpp>

#include "player_nd2.hpp"

namespace digitalcurling::players {

namespace {

PlayerND2Factory const& Validated(PlayerND2Factory const& factory)
{
    factory.Validate();
    return factory;
}

} // namespace

PlayerND2::PlayerND2(PlayerND2Factory const& factory)
    : factory_(Validated(factory)), engine_(std::nullopt), IPlayer()
{
    if (!factory_.seed.has_value()) {
        factory_.seed = std::random_device()();
    }
    engine_.emplace(factory_.seed.value());
}
PlayerND2::PlayerND2(PlayerND2Storage const& storage)
    : factory_(Validated(storage.factory)), engine_(Xoshiro256PlusPlus()), IPlayer()
{
    LoadEngine(storage.engine_data);
}

moves::Shot PlayerND2::Play(moves::Shot const& shot)
{
    PlayerND2Parameter const& dist_params = shot.angular_velocity > 0.f ? factory_.ccw : factory_.cw;
    float const max_speed = std::min(shot.translational_velocity, dist_params.max_speed);

    float const speed = max_speed + GenerateND2ribution(dist_params.stddev_speed);
    float const rotation = shot.angular_velocity + GenerateND2ribution(dist_params.stddev_rotation);
    float const angle = shot.release_angle + GenerateND2ribution(dist_params.stddev_angle);

    return moves::Shot { speed, rotation, angle };
}

std::unique_ptr<IPlayerStorage> PlayerND2::CreateStorage() const
{
    auto storage = std::make_unique<PlayerND2Storage>();
    Save(*storage);
    return storage;
}
void PlayerND2::Save(IPlayerStorage & storage) const
{
    auto & s = static_cast<PlayerND2Storage &>(storage);
    s.factory = factory_;

    std::ostringstream s_engine;
    s_engine << *engine_;
    s.engine_data = s_engine.str();
}
void PlayerND2::Load(IPlayerStorage const& storage)
{
    auto const& s = static_cast<PlayerND2Storage const&>(storage);
    s.factory.Validate();
    LoadEngine(s.engine_data);
    factory_ = s.factory;
}

void PlayerND2::LoadEngine(std::string const& engine_data)
{
    // Parse into a temporary so that a failure leaves this player unchanged.
    Xoshiro256PlusPlus engine;
    std::istringstream s_engle(engine_data);
    s_engle >> engine;
    if (!s_engle || !(s_engle >> std::ws).eof()) {
        throw std::invalid_argument("nd2: invalid engine_data");
    }
    *engine_ = engine;
}
float PlayerND2::GenerateND2ribution(float sigma)
{
    if (sigma == 0) return 0;

    using fixed = fpm::fixed<std::int32_t, std::int64_t, 25>;
    const std::uint32_t x1 = static_cast<std::uint32_t>((*engine_)() >> 40);
    const std::uint32_t x2 = static_cast<std::uint32_t>((*engine_)() >> 40);
    const fixed u1 = fixed{1} - fixed::from_fixed_point<24>(x1);
    const fixed u2 = fixed::from_fixed_point<24>(x2);

    const fixed z = fpm::sqrt(fixed{-2} * fpm::log(u1)) * fpm::cos(fixed::two_pi() * u2);
    return static_cast<float>(z) * sigma;
}

} // namespace digitalcurling::players
