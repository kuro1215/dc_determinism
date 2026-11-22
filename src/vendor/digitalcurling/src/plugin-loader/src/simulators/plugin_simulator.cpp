// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "digitalcurling/common.hpp"
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/simulators/plugin_simulator.hpp"
#include "digitalcurling/simulators/plugin_simulator_factory.hpp"
#include "digitalcurling/simulators/plugin_simulator_storage.hpp"

namespace digitalcurling::simulators {

template <typename Interface>
BasicPluginSimulator<Interface>::BasicPluginSimulator(std::string simulator_id, digitalcurling::plugins::HandleId instance_id, std::weak_ptr<OwnerResource> owner_resource)
    : plugins::WrapperBase<Interface>(std::move(simulator_id), std::move(instance_id), std::move(owner_resource))
      { }

template <typename Interface>
void BasicPluginSimulator<Interface>::SetStones(ISimulator::AllStones const& stones) {
    this->template ExecuteResourceFunc<void>([&](auto resource) {
        resource->set_stones.Execute(this->GetInstanceId(), stones);
    });
}
template <typename Interface>
void BasicPluginSimulator<Interface>::SetStone(StoneIndex index, std::optional<ISimulator::StoneState> const& stone) {
    auto const idx = index.GetIndex();
    this->template ExecuteResourceFunc<void>([&](auto resource) {
        resource->set_stone.Execute(this->GetInstanceId(), idx, stone);
    });
}
template <typename Interface>
void BasicPluginSimulator<Interface>::Simulate(float sheet_width) {
    this->template ExecuteResourceFunc<void>([&](auto resource) {
        resource->simulate.Execute(this->GetInstanceId(), sheet_width);
    });
}

template <typename Interface>
ISimulator::AllStones BasicPluginSimulator<Interface>::GetStones() const {
    return this->template ExecuteResourceFunc<ISimulator::AllStones>([&](auto resource) {
        return resource->get_stones.Execute(this->GetInstanceId());
    });
}
template <typename Interface>
std::unique_ptr<ISimulatorFactory> BasicPluginSimulator<Interface>::GetFactory() const {
    return this->template ExecuteResourceFunc<std::unique_ptr<ISimulatorFactory>>([&](auto resource) {
        auto id = resource->get_factory.Execute(this->GetInstanceId());
        return std::make_unique<PluginSimulatorFactory>(this->GetPluginId(), id, resource);
    });
}
template <typename Interface>
std::unique_ptr<ISimulatorStorage> BasicPluginSimulator<Interface>::CreateStorage() const {
    return this->template ExecuteResourceFunc<std::unique_ptr<ISimulatorStorage>>([&](auto resource) {
        auto id = resource->create_storage.Execute(nullptr);
        resource->save.Execute(this->GetInstanceId(), id);
        return std::make_unique<PluginSimulatorStorage>(this->GetPluginId(), id, resource);
    });
}
template <typename Interface>
void BasicPluginSimulator<Interface>::Save(ISimulatorStorage& storage) const {
    auto plugin_storage = dynamic_cast<PluginSimulatorStorage*>(&storage);
    if (!plugin_storage || this->GetPluginId() != plugin_storage->GetSimulatorId())
        throw std::runtime_error("Failed to cast ISimulatorStorage to PluginSimulatorImplStorage for simulator: " + this->GetPluginId());

    this->template ExecuteResourceFunc<void>([&](auto resource) {
        resource->save.Execute(this->GetInstanceId(), plugin_storage->GetInstanceId());
    });
}
template <typename Interface>
void BasicPluginSimulator<Interface>::Load(ISimulatorStorage const& storage) {
    auto plugin_storage = dynamic_cast<const PluginSimulatorStorage*>(&storage);
    if (!plugin_storage || this->GetPluginId() != plugin_storage->GetSimulatorId())
        throw std::runtime_error("Failed to cast ISimulatorStorage to PluginSimulatorImplStorage for simulator: " + this->GetPluginId());

    this->template ExecuteResourceFunc<void>([&](auto resource) {
        resource->load.Execute(this->GetInstanceId(), plugin_storage->GetInstanceId());
    });
}

// Explicit instantiation definitions; the matching (exported) `extern template` declarations are in the header.
template class BasicPluginSimulator<ISimulator>;
template class BasicPluginSimulator<IStepSimulator>;

void PluginStepSimulator::Step(int frames, float sheet_width) {
    ExecuteResourceFunc<void>([&](auto resource) {
        resource->step.Execute(GetInstanceId(), frames, sheet_width);
    });
}
bool PluginStepSimulator::AreAllStonesStopped() const {
    return ExecuteResourceFunc<bool>([&](auto resource) {
        return resource->are_all_stones_stopped.Execute(GetInstanceId());
    });
}
float PluginStepSimulator::GetSecondsPerFrame() const {
    return ExecuteResourceFunc<float>([&](auto resource) {
        return resource->get_seconds_per_frame.Execute(GetInstanceId());
    });
}

} // namespace digitalcurling::simulators
