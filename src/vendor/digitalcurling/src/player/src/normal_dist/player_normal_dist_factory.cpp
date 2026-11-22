// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

#include "player_normal_dist_factory.hpp"
#include "player_normal_dist.hpp"

namespace digitalcurling::players {

nlohmann::json PlayerNormalDistFactory::ToJson() const {
    nlohmann::json j;
    to_json(j, *this);
    return j;
}

std::unique_ptr<IPlayer> PlayerNormalDistFactory::CreatePlayer() const {
    return std::make_unique<PlayerNormalDist>(*this);
}
std::unique_ptr<IPlayerFactory> PlayerNormalDistFactory::Clone() const {
    return std::make_unique<PlayerNormalDistFactory>(*this);
}

void PlayerNormalDistFactory::Validate() const {
    auto const is_positive = [](float x) { return std::isfinite(x) && x > 0.f; };
    if (!is_positive(max_speed)) {
        throw std::invalid_argument("normal_dist: max_speed must be finite and greater than 0");
    }
    if (!is_positive(stddev_speed)) {
        throw std::invalid_argument("normal_dist: stddev_speed must be finite and greater than 0");
    }
    if (!is_positive(stddev_angle)) {
        throw std::invalid_argument("normal_dist: stddev_angle must be finite and greater than 0");
    }
}

// json
void to_json(nlohmann::json & j, PlayerNormalDistFactory const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["gender"] = v.gender;
    j["max_speed"] = v.max_speed;
    j["stddev_speed"] = v.stddev_speed;
    j["stddev_angle"] = v.stddev_angle;
    j["seed"] = v.seed;
}
void from_json(nlohmann::json const& j, PlayerNormalDistFactory & v) {
    PlayerNormalDistFactory parsed;
    j.at("gender").get_to(parsed.gender);
    j.at("max_speed").get_to(parsed.max_speed);
    j.at("stddev_speed").get_to(parsed.stddev_speed);
    j.at("stddev_angle").get_to(parsed.stddev_angle);
    if (j.contains("seed")) {
        j.at("seed").get_to(parsed.seed);
    } else {
        parsed.seed = std::nullopt;
    }
    parsed.Validate();
    v = std::move(parsed);
}

} // namespace digitalcurling::players
