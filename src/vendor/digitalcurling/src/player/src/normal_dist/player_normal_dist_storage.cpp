// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

#include "player_normal_dist_storage.hpp"
#include "player_normal_dist.hpp"

namespace digitalcurling::players {

nlohmann::json PlayerNormalDistStorage::ToJson() const {
    nlohmann::json j;
    to_json(j, *this);
    return j;
}

std::unique_ptr<IPlayer> PlayerNormalDistStorage::CreatePlayer() const {
    return std::make_unique<PlayerNormalDist>(*this);
}

// json
void to_json(nlohmann::json & j, PlayerNormalDistStorage const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["factory"] = v.factory;
    j["engine_data"] = v.engine_data;
    j["speed_dist_data"] = v.speed_dist_data;
    j["angle_dist_data"] = v.angle_dist_data;
}
void from_json(nlohmann::json const& j, PlayerNormalDistStorage & v) {
    PlayerNormalDistStorage parsed;
    j.at("factory").get_to(parsed.factory);
    j.at("engine_data").get_to(parsed.engine_data);
    j.at("speed_dist_data").get_to(parsed.speed_dist_data);
    j.at("angle_dist_data").get_to(parsed.angle_dist_data);
    if (parsed.engine_data.empty()) {
        throw std::invalid_argument("normal_dist: engine_data of a stored state must not be empty");
    }
    v = std::move(parsed);
}

} // namespace digitalcurling::players
