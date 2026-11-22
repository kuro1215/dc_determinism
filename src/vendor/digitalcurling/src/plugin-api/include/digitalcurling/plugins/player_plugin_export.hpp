// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines macros for exporting player plugins

#pragma once

#include <exception>
#include <stdexcept>

#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/players/i_player.hpp"
#include "digitalcurling/plugins/i_plugin_object.hpp"
#include "digitalcurling/plugins/data_object.h"
#include "digitalcurling/plugins/detail/plugin_exports.hpp"

/// @cond Doxygen_Suppress

namespace digitalcurling::plugins::detail {

// --- Player-specific wrappers ---
template <typename Player>
DigitalCurling_ErrorCode PlayerGetGenderImpl(PlayerHandle* player, DigitalCurling_Gender* out_gender, char** out_error)
{
    if (!player)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "PlayerGetGender: player handle is nullptr.", out_error);
    if (!out_gender)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "PlayerGetGender: out_gender is nullptr.", out_error);
    try {
        auto* player_ptr = dynamic_cast<Player*>(player);
        if (!player_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "PlayerGetGender: player handle has an unexpected type.", out_error);
        *out_gender = static_cast<DigitalCurling_Gender>(player_ptr->GetGender());
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "PlayerGetGender", out_error);
    }
}

template <typename Player>
DigitalCurling_ErrorCode PlayerPlayImpl(PlayerHandle* player, const DigitalCurling_Shot* shot, DigitalCurling_Shot* out_shot, char** out_error)
{
    if (!player)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "PlayerPlay: player handle is nullptr.", out_error);
    if (!shot)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "PlayerPlay: shot is nullptr.", out_error);
    if (!out_shot)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "PlayerPlay: out_shot is nullptr.", out_error);

    try {
        auto* player_ptr = dynamic_cast<Player*>(player);
        if (!player_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "PlayerPlay: player handle has an unexpected type.", out_error);
        auto s = digitalcurling::moves::Shot {
            shot->translational_velocity,
            shot->angular_velocity,
            shot->release_angle
        };
        auto result_shot = player_ptr->Play(s);

        out_shot->translational_velocity = result_shot.translational_velocity;
        out_shot->angular_velocity = result_shot.angular_velocity;
        out_shot->release_angle = result_shot.release_angle;
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "PlayerPlay", out_error);
    }
}

template <typename Factory>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2PlayerGetFactory(void* player, void** out, char** error) noexcept {
    if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "GetFactory: out handle is nullptr.", error);
    try {
        FactoryHandle* factory = nullptr;
        auto* player_box = V2Box(player, V2BoxKind::kPlayer);
        if (!player_box) return V2BadHandle(player, "GetFactory", error);
        const auto code = GetFactoryImpl<PluginType::player, Factory>(player_box->player, &factory, error);
        if (code != DIGITALCURLING_OK) return static_cast<DigitalCurling_PluginV2Status>(code);
        auto* complete = dynamic_cast<Factory*>(factory);
        if (!complete) { delete factory; return V2ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "GetFactory: factory type is invalid.", error); }
        auto box = std::make_unique<V2OwnedBox>(); box->object = complete; box->destroy = &DestroyObjectImpl<Factory>; auto* creator_interface = dynamic_cast<IPluginObjectCreator*>(complete); box->creator = creator_interface ? static_cast<CreatorHandle*>(creator_interface) : nullptr; box->factory = static_cast<FactoryHandle*>(complete); box->kind = static_cast<uint32_t>(V2BoxKind::kFactory); *out = box.release();
        return DIGITALCURLING_OK;
    } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetFactory: unknown exception.", error); }
}
template <typename Storage>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2PlayerSave(void* player, void* storage, char** error) noexcept {
    try {
        auto* player_box = V2Box(player, V2BoxKind::kPlayer);
        if (!player_box) return V2BadHandle(player, "Save", error);
        auto* storage_box = V2Box(storage, V2BoxKind::kStorage);
        if (!storage_box) return V2BadHandle(storage, "Save", error);
        return static_cast<DigitalCurling_PluginV2Status>(TargetSaveImpl<PluginType::player, Storage>(player_box->player, storage_box->storage, error));
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Save: unknown exception.", error); }
}
template <typename Storage>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2PlayerLoad(void* player, void* storage, char** error) noexcept {
    try {
        auto* player_box = V2Box(player, V2BoxKind::kPlayer);
        if (!player_box) return V2BadHandle(player, "Load", error);
        auto* storage_box = V2Box(storage, V2BoxKind::kStorage);
        if (!storage_box) return V2BadHandle(storage, "Load", error);
        return static_cast<DigitalCurling_PluginV2Status>(TargetLoadImpl<PluginType::player, Storage>(player_box->player, storage_box->storage, error));
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Load: unknown exception.", error); }
}
template <typename Player>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2PlayerGender(void* player, int32_t* out, char** error) noexcept {
    if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "GetGender: out is nullptr.", error);
    try {
        DigitalCurling_Gender value{};
        auto* box = V2Box(player, V2BoxKind::kPlayer);
        if (!box) return V2BadHandle(player, "GetGender", error);
        const auto code = PlayerGetGenderImpl<Player>(box->player, &value, error);
        if (code == DIGITALCURLING_OK) *out = static_cast<int32_t>(value);
        return static_cast<DigitalCurling_PluginV2Status>(code);
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetGender: unknown exception.", error); }
}
template <typename Player>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2PlayerPlay(void* player, const DigitalCurling_PluginV2Shot* shot, DigitalCurling_PluginV2Shot* out, char** error) noexcept {
    try {
        auto* box = V2Box(player, V2BoxKind::kPlayer);
        if (!box) return V2BadHandle(player, "Play", error);
        return static_cast<DigitalCurling_PluginV2Status>(PlayerPlayImpl<Player>(box->player, reinterpret_cast<const DigitalCurling_Shot*>(shot), reinterpret_cast<DigitalCurling_Shot*>(out), error));
    }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Play: unknown exception.", error); }
}


} // namespace

/// @endcond

// --- Export Macros for Users ---
/// @brief Macro for exporting a player plugin
#define DIGITALCURLING_EXPORT_PLAYER_PLUGIN(FactoryClass, StorageClass, PlayerClass) \
    static_assert(std::is_base_of_v<digitalcurling::players::IPlayerFactory, FactoryClass>, "FactoryClass must derive from IPlayerFactory"); \
    static_assert(std::is_base_of_v<digitalcurling::players::IPlayerStorage, StorageClass>, "StorageClass must derive from IPlayerStorage"); \
    static_assert(std::is_base_of_v<digitalcurling::players::IPlayer, PlayerClass>, "PlayerClass must derive from IPlayer"); \
    \
    static const DigitalCurling_PluginV2PlayerApi g_player_api_v2_instance = { \
        &digitalcurling::plugins::detail::V2PlayerGetFactory<FactoryClass>, \
        &digitalcurling::plugins::detail::V2PlayerSave<StorageClass>, \
        &digitalcurling::plugins::detail::V2PlayerLoad<StorageClass>, \
        &digitalcurling::plugins::detail::V2PlayerGender<PlayerClass>, \
        &digitalcurling::plugins::detail::V2PlayerPlay<PlayerClass> \
    }; \
    DIGITALCURLING_EXPORT_PLUGIN_INNER(digitalcurling::plugins::PluginType::player, FactoryClass, StorageClass, PlayerClass, &g_player_api_v2_instance, static_cast<uint32_t>(sizeof(DigitalCurling_PluginV2PlayerApi)), nullptr, 0u)
