// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "digitalcurling/players/plugin_player.hpp"
#include "digitalcurling/players/plugin_player_factory.hpp"
#include "digitalcurling/players/plugin_player_storage.hpp"
#include "digitalcurling/plugins/handle_id.hpp"

namespace digitalcurling::players {

PluginPlayer::PluginPlayer(std::string player_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
    : WrapperBase(std::move(player_id), std::move(instance_id), std::move(owner_resource))
      {}

moves::Shot PluginPlayer::Play(moves::Shot const& shot) {
    return ExecuteResourceFunc<moves::Shot>([&](auto resource) {
        return resource->play.Execute(GetInstanceId(), shot);
    });
}

Gender PluginPlayer::GetGender() const {
    return ExecuteResourceFunc<Gender>([&](auto resource) {
        return resource->get_gender.Execute(GetInstanceId());
    });
}

std::unique_ptr<IPlayerFactory> PluginPlayer::GetFactory() const {
    return ExecuteResourceFunc<std::unique_ptr<IPlayerFactory>>([&](auto resource) {
        auto id = resource->get_factory.Execute(GetInstanceId());
        return std::make_unique<PluginPlayerFactory>(GetPluginId(), id, resource);
    });
}
std::unique_ptr<IPlayerStorage> PluginPlayer::CreateStorage() const {
    return ExecuteResourceFunc<std::unique_ptr<IPlayerStorage>>([&](auto resource) {
        auto id = resource->create_storage.Execute(nullptr);
        resource->save.Execute(GetInstanceId(), id);
        return std::make_unique<PluginPlayerStorage>(GetPluginId(), id, resource);
    });
}

void PluginPlayer::Save(IPlayerStorage& storage) const {
    auto plugin_storage = dynamic_cast<PluginPlayerStorage*>(&storage);
    if (!plugin_storage || GetPluginId() != plugin_storage->GetPlayerId())
        throw std::runtime_error("Failed to cast IPlayerStorage to PluginPlayerStorage for player: " + GetPluginId());

    ExecuteResourceFunc<void>([&](auto resource) {
        resource->save.Execute(GetInstanceId(), plugin_storage->GetInstanceId());
    });
}
void PluginPlayer::Load(IPlayerStorage const& storage) {
    auto plugin_storage = dynamic_cast<const PluginPlayerStorage*>(&storage);
    if (!plugin_storage || GetPluginId() != plugin_storage->GetPlayerId())
        throw std::runtime_error("Failed to cast IPlayerStorage to PluginPlayerStorage for player: " + GetPluginId());

    ExecuteResourceFunc<void>([&](auto resource) {
        resource->load.Execute(GetInstanceId(), plugin_storage->GetInstanceId());
    });
}

} // namespace digitalcurling
