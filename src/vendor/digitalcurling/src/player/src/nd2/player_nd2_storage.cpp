// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "digitalcurling/common.hpp"

#include "player_nd2_storage.hpp"
#include "player_nd2.hpp"
#include "xoshiro256plusplus.hpp"

namespace digitalcurling::players {

std::string PlayerND2Storage::DefaultEngineData() {
    std::ostringstream stream;
    stream << Xoshiro256PlusPlus();
    return stream.str();
}

nlohmann::json PlayerND2Storage::ToJson() const {
    nlohmann::json j;
    to_json(j, *this);
    return j;
}

std::unique_ptr<IPlayer> PlayerND2Storage::CreatePlayer() const {
    return std::make_unique<PlayerND2>(*this);
}

// json
void to_json(nlohmann::json & j, PlayerND2Storage const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["factory"] = v.factory;
    j["engine_data"] = v.engine_data;
}
void from_json(nlohmann::json const& j, PlayerND2Storage & v) {
    j.at("factory").get_to(v.factory);
    j.at("engine_data").get_to(v.engine_data);
}

} // namespace digitalcurling::players
