// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include "digitalcurling/plugins/detail/native_loader.hpp"
#include "digitalcurling/plugins/detail/plugin_resource.hpp"
#ifdef DIGITALCURLING_LOADER_TEST_SUPPORT
#include "digitalcurling_plugin_loader_export.h"
#endif

#define DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(func_ptr) \
    do { \
        if (!(func_ptr)) { \
            throw std::runtime_error("PluginResource: Essential function '" #func_ptr "' is missing from the plugin API."); \
        } \
    } while (0)

namespace digitalcurling::plugins::detail {

#ifdef DIGITALCURLING_LOADER_TEST_SUPPORT
DIGITALCURLING_LOADER_API std::shared_ptr<PluginResource> CreateTestPluginResource(
    const std::filesystem::path& path, bool as_builtin) {
    auto native = LoadNativePlugin(path);
    if (as_builtin) {
        // Keep the descriptor's module alive while presenting a built-in identity.
        native.handle = ModulePtr(native.handle, nullptr);
    }
    return PluginResource::CreateV2(native.get_plugin_api_v2(), std::move(native.handle));
}
#endif

PluginResource::PluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle,
                               std::optional<uint32_t> capabilities, CreatorCallback create_factory_callback, CreatorCallback create_storage_callback)
    : info_(std::move(info)),
      api_(std::move(api)),
      handle_(std::move(handle)),
      instance_list_(),
      // The STEP capability is only decided from the plugin descriptor (CreateV2): the host adapter's
      // step callbacks are always set, so they cannot tell whether the plugin supports stepping.
      capabilities_(capabilities.value_or(api.simulator && api.simulator->calculate_shot ? DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT : 0u)),
      create_factory(create_factory_callback ? PluginFunction<CreateFactoryFunc, digitalcurling::plugins::HandleId>(std::move(create_factory_callback), instance_list_) : PluginFunction<CreateFactoryFunc, digitalcurling::plugins::HandleId>(api.create_factory, api.free_string, api.destroy_factory, instance_list_)),
      create_storage(create_storage_callback ? PluginFunction<CreateStorageFunc, digitalcurling::plugins::HandleId>(std::move(create_storage_callback), instance_list_) : PluginFunction<CreateStorageFunc, digitalcurling::plugins::HandleId>(api.create_storage, api.free_string, api.destroy_storage, instance_list_)),
      create_target(api.create_target, api.free_string, api.destroy_target, instance_list_),
      object_creator_get_state(api.object_creator_get_state, api.free_string, instance_list_),
      factory_set_state(api.factory_set_state, api.free_string, instance_list_),
      storage_set_state(api.storage_set_state, api.free_string, instance_list_)
{
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(api.free_string);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(api.destroy_factory);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(create_factory);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(create_storage);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(create_target);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(object_creator_get_state);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(factory_set_state);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(storage_set_state);
}
PluginResource::~PluginResource()
{
    instance_list_.Clear();
}
bool PluginResource::IsSameModule(const PluginResource& other) const {
    if (this == &other) return true;
    const auto handle = GetModuleHandle();
    return handle != nullptr && handle == other.GetModuleHandle();
}

PlayerPluginResource::PlayerPluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle,
                                           CreatorCallback create_factory_callback, CreatorCallback create_storage_callback, std::optional<uint32_t> capabilities)
    : PluginResource(std::move(info), api, std::move(handle), capabilities, std::move(create_factory_callback), std::move(create_storage_callback)),
      get_factory(api.player->get_factory, api.free_string, api.destroy_factory, instance_list_),
      save(api.player->save, api.free_string, instance_list_),
      load(api.player->load, api.free_string, instance_list_),
      get_gender(api.player->get_gender, api.free_string, instance_list_),
      play(api.player->play, api.free_string, instance_list_)
{
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(get_factory);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(save);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(load);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(get_gender);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(play);
}

SimulatorPluginResource::SimulatorPluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle,
                                                 CreatorCallback create_factory_callback, CreatorCallback create_storage_callback, std::optional<uint32_t> capabilities)
    : PluginResource(std::move(info), api, std::move(handle), capabilities, std::move(create_factory_callback), std::move(create_storage_callback)),
      get_factory(api.simulator->get_factory, api.free_string, api.destroy_factory, instance_list_),
      save(api.simulator->save, api.free_string, instance_list_),
      load(api.simulator->load, api.free_string, instance_list_),
      step(api.simulator->step, api.free_string, instance_list_),
      simulate(api.simulator->simulate, api.free_string, instance_list_),
      set_stones(api.simulator->set_stones, api.free_string, instance_list_),
      set_stone(api.simulator->set_stone, api.free_string, instance_list_),
      are_all_stones_stopped(api.simulator->are_all_stones_stopped, api.free_string, instance_list_),
      get_stones(api.simulator->get_stones, api.free_string, instance_list_),
      get_seconds_per_frame(api.simulator->get_seconds_per_frame, api.free_string, instance_list_),
      calculate_shot(api.simulator->calculate_shot, api.free_string, instance_list_)
{
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(get_factory);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(save);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(load);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(simulate);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(set_stones);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(set_stone);
    DIGITALCURLING_PLUGIN_LOADER_CHECK_VALID_FUNC(get_stones);
}

} // namespace digitalcurling::plugins::detail
