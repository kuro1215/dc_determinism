// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#define DIGITALCURLING_PLUGIN_NAME "core04_long_name_player_plugin_fixture_0123456789_abcdefghijklmnopqrstuvwxyz_0123456789_abcdefghijklmnopqrstuvwxyz_end"

#include <memory>
#include <nlohmann/json.hpp>
#include "digitalcurling/players/gender.hpp"
#include "digitalcurling/players/i_player.hpp"
#include "digitalcurling/players/i_player_factory.hpp"
#include "digitalcurling/players/i_player_storage.hpp"
#include "digitalcurling/plugins/player_plugin_export.hpp"

namespace digitalcurling::players {

class LongNameFixturePlayerFactory;
class LongNameFixturePlayerStorage;

class LongNameFixturePlayer final : public IPlayer {
public:
    explicit LongNameFixturePlayer(Gender gender) : gender_(gender) {}
    ~LongNameFixturePlayer() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    Gender GetGender() const override { return gender_; }
    std::unique_ptr<IPlayerFactory> GetFactory() const override;
    moves::Shot Play(moves::Shot const& shot) override { return shot; }
    std::unique_ptr<IPlayerStorage> CreateStorage() const override;
    void Save(IPlayerStorage& storage) const override;
    void Load(IPlayerStorage const& storage) override;

private:
    Gender gender_ = Gender::kUnknown;
};

class LongNameFixturePlayerFactory final : public IPlayerFactory {
public:
    Gender gender = Gender::kUnknown;

    LongNameFixturePlayerFactory() = default;
    ~LongNameFixturePlayerFactory() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    nlohmann::json ToJson() const override {
        return nlohmann::json{{"type", DIGITALCURLING_PLUGIN_NAME}, {"gender", gender}};
    }
    Gender GetGender() const override { return gender; }
    std::unique_ptr<IPlayer> CreatePlayer() const override {
        return std::make_unique<LongNameFixturePlayer>(gender);
    }
    std::unique_ptr<IPlayerFactory> Clone() const override {
        return std::make_unique<LongNameFixturePlayerFactory>(*this);
    }
};

class LongNameFixturePlayerStorage final : public IPlayerStorage {
public:
    Gender gender = Gender::kUnknown;

    LongNameFixturePlayerStorage() = default;
    ~LongNameFixturePlayerStorage() override = default;

    const char* GetId() const noexcept override { return DIGITALCURLING_PLUGIN_NAME; }
    nlohmann::json ToJson() const override {
        return nlohmann::json{{"type", DIGITALCURLING_PLUGIN_NAME}, {"gender", gender}};
    }
    Gender GetGender() const override { return gender; }
    std::unique_ptr<IPlayer> CreatePlayer() const override {
        return std::make_unique<LongNameFixturePlayer>(gender);
    }
};

inline void to_json(nlohmann::json& j, LongNameFixturePlayerFactory const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["gender"] = v.gender;
}

inline void from_json(nlohmann::json const& j, LongNameFixturePlayerFactory& v) {
    if (j.contains("gender")) {
        j.at("gender").get_to(v.gender);
    }
}

inline void to_json(nlohmann::json& j, LongNameFixturePlayerStorage const& v) {
    j["type"] = DIGITALCURLING_PLUGIN_NAME;
    j["gender"] = v.gender;
}

inline void from_json(nlohmann::json const& j, LongNameFixturePlayerStorage& v) {
    if (j.contains("gender")) {
        j.at("gender").get_to(v.gender);
    }
}

std::unique_ptr<IPlayerFactory> LongNameFixturePlayer::GetFactory() const {
    auto factory = std::make_unique<LongNameFixturePlayerFactory>();
    factory->gender = gender_;
    return factory;
}

std::unique_ptr<IPlayerStorage> LongNameFixturePlayer::CreateStorage() const {
    auto storage = std::make_unique<LongNameFixturePlayerStorage>();
    Save(*storage);
    return storage;
}

void LongNameFixturePlayer::Save(IPlayerStorage& storage) const {
    static_cast<LongNameFixturePlayerStorage&>(storage).gender = gender_;
}

void LongNameFixturePlayer::Load(IPlayerStorage const& storage) {
    gender_ = static_cast<LongNameFixturePlayerStorage const&>(storage).gender;
}

} // namespace digitalcurling::players

using digitalcurling::players::LongNameFixturePlayer;
using digitalcurling::players::LongNameFixturePlayerFactory;
using digitalcurling::players::LongNameFixturePlayerStorage;

DIGITALCURLING_EXPORT_PLAYER_PLUGIN(
    LongNameFixturePlayerFactory, LongNameFixturePlayerStorage, LongNameFixturePlayer
)
