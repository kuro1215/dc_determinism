// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

#include "player_nd2_factory.hpp"
#include "player_nd2.hpp"

namespace digitalcurling::players {

nlohmann::json PlayerND2Factory::ToJson() const {
    nlohmann::json j;
    to_json(j, *this);
    return j;
}

std::unique_ptr<IPlayer> PlayerND2Factory::CreatePlayer() const {
    return std::make_unique<PlayerND2>(*this);
}
std::unique_ptr<IPlayerFactory> PlayerND2Factory::Clone() const {
    return std::make_unique<PlayerND2Factory>(*this);
}

namespace {

void ValidateParameter(PlayerND2Parameter const& p, std::string const& name) {
    if (!std::isfinite(p.max_speed) || !(p.max_speed > 0.f)) {
        throw std::invalid_argument("nd2: " + name + ".max_speed must be finite and greater than 0");
    }
    auto const is_valid_stddev = [](float x) { return std::isfinite(x) && x >= 0.f; };
    if (!is_valid_stddev(p.stddev_speed)) {
        throw std::invalid_argument("nd2: " + name + ".stddev_speed must be finite and at least 0");
    }
    if (!is_valid_stddev(p.stddev_angle)) {
        throw std::invalid_argument("nd2: " + name + ".stddev_angle must be finite and at least 0");
    }
    if (!is_valid_stddev(p.stddev_rotation)) {
        throw std::invalid_argument("nd2: " + name + ".stddev_rotation must be finite and at least 0");
    }
}

} // namespace

void PlayerND2Factory::Validate() const {
    ValidateParameter(cw, "cw");
    ValidateParameter(ccw, "ccw");
}

// json
void to_json(nlohmann::json & j, PlayerND2Factory const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["gender"] = v.gender;
    j["cw"] = v.cw;
    j["ccw"] = v.ccw;
    j["seed"] = v.seed;
}
void from_json(nlohmann::json const& j, PlayerND2Factory & v) {
    PlayerND2Factory parsed;
    j.at("gender").get_to(parsed.gender);
    j.at("cw").get_to(parsed.cw);
    j.at("ccw").get_to(parsed.ccw);
    try_get_to(j, "seed", parsed.seed, std::nullopt);
    parsed.Validate();
    v = std::move(parsed);
}

} // namespace digitalcurling::players
