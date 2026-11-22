// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/plugins/plugin_type.hpp"
#include "digitalcurling/plugins/detail/native_loader.hpp"
#include "digitalcurling/plugins/detail/plugin_function.hpp"
#include "digitalcurling/plugins/plugin_api_v2.h"

namespace digitalcurling::plugins::detail {

class PlayerPluginResource;
class SimulatorPluginResource;

class PluginResource {
public:
    PluginResource(const PluginResource&) = delete;
    PluginResource& operator=(const PluginResource&) = delete;
    PluginResource(PluginResource&&) = delete;
    PluginResource& operator=(PluginResource&&) = delete;

    virtual ~PluginResource();

    static std::shared_ptr<PluginResource> CreateV2(const DigitalCurling_PluginApiV2* api, std::optional<ModulePtr> handle);

    PluginType GetType() const { return static_cast<PluginType>(info_.plugin_type); }
    std::string GetName() const { return info_.plugin_name; }
    const PluginInfo& GetInfo() const noexcept { return info_; }
    const PluginApi& GetApi() const noexcept { return api_; }
    const void* GetModuleHandle() const noexcept { return (handle_.has_value() && *handle_) ? handle_->get() : nullptr; }
    bool IsBuiltIn() const noexcept { return GetModuleHandle() == nullptr; }
    bool IsSameModule(const PluginResource& other) const;
    PluginInstanceList& GetInstanceList() { return instance_list_; }
    uint32_t GetCapabilities() const { return capabilities_; }

    const PluginFunction<CreateFactoryFunc, HandleId> create_factory;
    const PluginFunction<CreateStorageFunc, HandleId> create_storage;
    const PluginFunction<CreateTargetFunc, HandleId> create_target;
    const PluginFunction<ObjectCreatorGetStateFunc, std::string> object_creator_get_state;
    const PluginFunction<FactorySetStateFunc, void> factory_set_state;
    const PluginFunction<StorageSetStateFunc, void> storage_set_state;

protected:
    using CreatorCallback = PluginFunction<CreateFactoryFunc, HandleId>::CreateCallback;
    PluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle, std::optional<uint32_t> capabilities,
                   CreatorCallback create_factory_callback = {}, CreatorCallback create_storage_callback = {});

    PluginInfo info_;
    PluginApi api_;
    std::optional<ModulePtr> handle_;
    PluginInstanceList instance_list_;
    uint32_t capabilities_;
};

class PlayerPluginResource final : public PluginResource {
public:
    const PluginFunction<PlayerGetFactoryFunc, HandleId> get_factory;
    const PluginFunction<PlayerSaveFunc, void> save;
    const PluginFunction<PlayerLoadFunc, void> load;

    const PluginFunction<PlayerGetGenderFunc, players::Gender> get_gender;
    const PluginFunction<PlayerPlayFunc, moves::Shot> play;

    explicit PlayerPluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle,
                                  CreatorCallback create_factory_callback = {}, CreatorCallback create_storage_callback = {}, std::optional<uint32_t> capabilities = std::nullopt);
};


class SimulatorPluginResource final : public PluginResource {
public:
    const PluginFunction<SimulatorGetFactoryFunc, HandleId> get_factory;
    const PluginFunction<SimulatorSaveFunc, void> save;
    const PluginFunction<SimulatorLoadFunc, void> load;

    const PluginFunction<SimulatorStepFunc, void> step;
    const PluginFunction<SimulatorSimulateFunc, void> simulate;
    const PluginFunction<SimulatorSetStonesFunc, void> set_stones;
    const PluginFunction<SimulatorSetStoneFunc, void> set_stone;

    const PluginFunction<SimulatorAreAllStonesStoppedFunc, bool> are_all_stones_stopped;
    const PluginFunction<SimulatorGetStonesFunc, simulators::ISimulator::AllStones> get_stones;
    const PluginFunction<SimulatorGetSecondsPerFrameFunc, float> get_seconds_per_frame;

    const PluginFunction<SimulatorCalculateShotFunc, moves::Shot> calculate_shot;

    explicit SimulatorPluginResource(PluginInfo info, PluginApi api, std::optional<ModulePtr> handle,
                                     CreatorCallback create_factory_callback = {}, CreatorCallback create_storage_callback = {}, std::optional<uint32_t> capabilities = std::nullopt);

    bool IsStepSimulator() const {
        return (GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_STEP) != 0;
    }

    bool IsInvertibleSimulator() const {
        return (GetCapabilities() & DIGITALCURLING_PLUGIN_CAPABILITY_CALCULATE_SHOT) != 0;
    }
};

} // namespace digitalcurling::plugins::detail
