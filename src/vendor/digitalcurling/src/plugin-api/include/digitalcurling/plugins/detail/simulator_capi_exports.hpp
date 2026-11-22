// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <exception>
#include <optional>
#include <type_traits>
#include <nlohmann/json.hpp>

#include "digitalcurling/stone_coordinate.hpp"
#include "digitalcurling/stone_index.hpp"
#include "digitalcurling/vector2.hpp"
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_simulator_storage.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"
#include "digitalcurling/plugins/data_object.h"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/plugins/detail/plugin_exports.hpp"

namespace digitalcurling::plugins::detail {


// --- Simulator-specific wrappers ---
// `Simulator` must derive from IStepSimulator for the step-capability wrappers
// (SimulatorStepImpl, SimulatorAreAllStonesStoppedImpl, SimulatorGetSecondsPerFrameImpl).
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorStepImpl(SimulatorHandle* sim, const int frames, const float sheet_width, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorStep: simulator handle is nullptr.", out_error);
    if (frames <= 0)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorStep: step frames must be positive.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorStep: simulator handle has an unexpected type.", out_error);
        digitalcurling::simulators::StepUntilRest(*sim_ptr, frames, sheet_width);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorStep", out_error);
    }
}
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorSimulateImpl(SimulatorHandle* sim, const float sheet_width, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorSimulate: simulator handle is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorSimulate: simulator handle has an unexpected type.", out_error);
        sim_ptr->Simulate(sheet_width);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorSimulate", out_error);
    }
}
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorSetStonesImpl(SimulatorHandle* sim, const DigitalCurling_StoneCoordinate* stones, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorSetStones: simulator handle is nullptr.", out_error);
    if (!stones)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorSetStones: stones is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorSetStones: simulator handle has an unexpected type.", out_error);
        digitalcurling::simulators::ISimulator::AllStones stones_data;
        for (int i = 0; i < digitalcurling::StoneCoordinate::kStoneMax; i++) {
            auto struct_stone = stones->stones[i];
            if (struct_stone.position.x == 0.f && struct_stone.position.y == 0.f && struct_stone.angle == 0.f &&
                struct_stone.translational_velocity.x == 0.f && struct_stone.translational_velocity.y == 0.f && struct_stone.angular_velocity == 0.f)
            {
                stones_data[i] = std::nullopt;
            } else {
                stones_data[i] = digitalcurling::simulators::ISimulator::StoneState{
                    digitalcurling::Vector2(struct_stone.position.x, struct_stone.position.y),
                    struct_stone.angle,
                    digitalcurling::Vector2(struct_stone.translational_velocity.x, struct_stone.translational_velocity.y),
                    struct_stone.angular_velocity
                };
            }
        }
        sim_ptr->SetStones(stones_data);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorSetStones", out_error);
    }
}
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorSetStoneImpl(SimulatorHandle* sim, const unsigned char stone_index, const DigitalCurling_StoneState* stone, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorSetStone: simulator handle is nullptr.", out_error);

    if (stone_index >= digitalcurling::StoneCoordinate::kStoneMax)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorSetStone: stone_index is out of range.", out_error);
    digitalcurling::StoneIndex idx(stone_index);

    try {
        std::optional<digitalcurling::simulators::ISimulator::StoneState> stone_data = std::nullopt;
        if (stone) {
            stone_data = digitalcurling::simulators::ISimulator::StoneState{
                digitalcurling::Vector2(stone->position.x, stone->position.y),
                stone->angle,
                digitalcurling::Vector2(stone->translational_velocity.x, stone->translational_velocity.y),
                stone->angular_velocity
            };
        }
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorSetStone: simulator handle has an unexpected type.", out_error);
        sim_ptr->SetStone(idx, stone_data);
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorSetStone", out_error);
    }
}

template <typename Simulator>
DigitalCurling_ErrorCode SimulatorAreAllStonesStoppedImpl(SimulatorHandle* sim, bool* out_stopped, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorAreAllStonesStopped: simulator handle is nullptr.", out_error);
    if (!out_stopped)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "SimulatorAreAllStonesStopped: out_stopped is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorAreAllStonesStopped: simulator handle has an unexpected type.", out_error);
        *out_stopped = sim_ptr->AreAllStonesStopped();
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorAreAllStonesStopped", out_error);
    }
}
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorGetStonesImpl(SimulatorHandle* sim, DigitalCurling_StoneCoordinate* out_stones, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorGetStones: simulator handle is nullptr.", out_error);
    if (!out_stones)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "SimulatorGetStones: out_stones is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorGetStones: simulator handle has an unexpected type.", out_error);
        auto stones = sim_ptr->GetStones();
        for (int i = 0; i < digitalcurling::StoneCoordinate::kStoneMax; i++) {
            if (stones[i].has_value()) {
                out_stones->stones[i].position.x = stones[i]->position.x;
                out_stones->stones[i].position.y = stones[i]->position.y;
                out_stones->stones[i].angle = stones[i]->angle;
                out_stones->stones[i].translational_velocity.x = stones[i]->translational_velocity.x;
                out_stones->stones[i].translational_velocity.y = stones[i]->translational_velocity.y;
                out_stones->stones[i].angular_velocity = stones[i]->angular_velocity;
            } else {
                out_stones->stones[i].position.x = 0.f;
                out_stones->stones[i].position.y = 0.f;
                out_stones->stones[i].angle = 0.f;
                out_stones->stones[i].translational_velocity.x = 0.f;
                out_stones->stones[i].translational_velocity.y = 0.f;
                out_stones->stones[i].angular_velocity = 0.f;
            }
        }
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorGetStones", out_error);
    }
}
template <typename Simulator>
DigitalCurling_ErrorCode SimulatorGetSecondsPerFrameImpl(SimulatorHandle* sim, float* out_seconds, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorGetSecondsPerFrame: simulator handle is nullptr.", out_error);
    if (!out_seconds)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorGetSecondsPerFrame: out_seconds is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorGetSecondsPerFrame: simulator handle has an unexpected type.", out_error);
        *out_seconds = sim_ptr->GetSecondsPerFrame();
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorGetSecondsPerFrame", out_error);
    }
}

template <typename Simulator, moves::Shot (Simulator::*CalcShotFunc)(Vector2 const&, float, float) const>
DigitalCurling_ErrorCode SimulatorCalculateShotImpl(SimulatorHandle* sim, const DigitalCurling_Vector2* target_position, const float target_speed, const float angular_velocity,
                                       DigitalCurling_Shot* out_shot, char** out_error)
{
    if (!sim)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorCalculateShot: simulator handle is nullptr.", out_error);
    if (!target_position)
        return ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SimulatorCalculateShot: target_position is nullptr.", out_error);
    if (!out_shot)
        return ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "SimulatorCalculateShot: out_shot is nullptr.", out_error);

    try {
        auto* sim_ptr = dynamic_cast<Simulator*>(sim);
        if (!sim_ptr)
            return ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "SimulatorCalculateShot: simulator handle has an unexpected type.", out_error);
        auto shot = (sim_ptr->*CalcShotFunc)({target_position->x, target_position->y}, target_speed, angular_velocity);

        out_shot->translational_velocity = shot.translational_velocity;
        out_shot->angular_velocity = shot.angular_velocity;
        out_shot->release_angle = shot.release_angle;
        return DIGITALCURLING_OK;
    } catch (const std::exception& e) {
        return ReturnException(e, "SimulatorCalculateShot", out_error);
    }
}

template <typename Factory>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorGetFactory(void* sim, void** out, char** error) noexcept {
    if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "GetFactory: out handle is nullptr.", error);
    try { FactoryHandle* factory = nullptr; auto* sim_box = V2Box(sim, V2BoxKind::kSimulator); if (!sim_box) return V2BadHandle(sim, "GetFactory", error); const auto code = GetFactoryImpl<PluginType::simulator, Factory>(sim_box->simulator, &factory, error); if (code != DIGITALCURLING_OK) return static_cast<DigitalCurling_PluginV2Status>(code); auto* complete = dynamic_cast<Factory*>(factory); if (!complete) { delete factory; return V2ReturnError(DIGITALCURLING_ERR_FAILED_TO_CAST, "GetFactory: factory type is invalid.", error); } auto box = std::make_unique<V2OwnedBox>(); box->object = complete; box->destroy = &DestroyObjectImpl<Factory>; auto* creator_interface = dynamic_cast<IPluginObjectCreator*>(complete); box->creator = creator_interface ? static_cast<CreatorHandle*>(creator_interface) : nullptr; box->factory = static_cast<FactoryHandle*>(complete); box->kind = static_cast<uint32_t>(V2BoxKind::kFactory); *out = box.release(); return DIGITALCURLING_OK; }
    catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetFactory: unknown exception.", error); }
}
template <typename Storage> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorSave(void* sim, void* storage, char** error) noexcept { try { auto* a=V2Box(sim, V2BoxKind::kSimulator); if (!a) return V2BadHandle(sim, "Save", error); auto* b=V2Box(storage, V2BoxKind::kStorage); if (!b) return V2BadHandle(storage, "Save", error); return static_cast<DigitalCurling_PluginV2Status>(TargetSaveImpl<PluginType::simulator, Storage>(a->simulator, b->storage, error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Save: unknown exception.", error); } }
template <typename Storage> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorLoad(void* sim, void* storage, char** error) noexcept { try { auto* a=V2Box(sim, V2BoxKind::kSimulator); if (!a) return V2BadHandle(sim, "Load", error); auto* b=V2Box(storage, V2BoxKind::kStorage); if (!b) return V2BadHandle(storage, "Load", error); return static_cast<DigitalCurling_PluginV2Status>(TargetLoadImpl<PluginType::simulator, Storage>(a->simulator, b->storage, error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Load: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorStep(void* sim, int32_t frames, float width, char** error) noexcept { try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "Step", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorStepImpl<Sim>(b->simulator, frames, width, error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Step: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorSimulate(void* sim, float width, char** error) noexcept { try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "Simulate", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorSimulateImpl<Sim>(b->simulator, width, error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "Simulate: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorSetStones(void* sim, const DigitalCurling_PluginV2StoneCoordinate* stones, char** error) noexcept { try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "SetStones", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorSetStonesImpl<Sim>(b->simulator, reinterpret_cast<const DigitalCurling_StoneCoordinate*>(stones), error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "SetStones: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorSetStone(void* sim, uint32_t index, const DigitalCurling_PluginV2StoneState* stone, char** error) noexcept { if (index > 255u) return V2ReturnError(DIGITALCURLING_ERR_INVALID_ARGUMENT, "SetStone: index is out of range.", error); try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "SetStone", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorSetStoneImpl<Sim>(b->simulator, static_cast<unsigned char>(index), reinterpret_cast<const DigitalCurling_StoneState*>(stone), error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "SetStone: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorStopped(void* sim, DigitalCurling_PluginV2Bool* out, char** error) noexcept { if (!out) return V2ReturnError(DIGITALCURLING_ERR_BUFFER_NULLPTR, "AreStopped: out is nullptr.", error); try { bool value{}; auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "AreStopped", error); const auto code = SimulatorAreAllStonesStoppedImpl<Sim>(b->simulator, &value, error); if (code == DIGITALCURLING_OK) *out = value ? 1u : 0u; return static_cast<DigitalCurling_PluginV2Status>(code); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "AreStopped: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorGetStones(void* sim, DigitalCurling_PluginV2StoneCoordinate* out, char** error) noexcept { try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "GetStones", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorGetStonesImpl<Sim>(b->simulator, reinterpret_cast<DigitalCurling_StoneCoordinate*>(out), error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetStones: unknown exception.", error); } }
template <typename Sim> DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorSeconds(void* sim, float* out, char** error) noexcept { try { auto* b=V2Box(sim, V2BoxKind::kSimulator); if (!b) return V2BadHandle(sim, "GetSeconds", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorGetSecondsPerFrameImpl<Sim>(b->simulator, out, error)); } catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "GetSeconds: unknown exception.", error); } }

// The optional step callbacks are exported only for step-capable simulators (all three or none).
template <typename Sim> constexpr DigitalCurling_PluginV2Step V2SimulatorStepEntry() noexcept {
    if constexpr (std::is_base_of_v<digitalcurling::simulators::IStepSimulator, Sim>) { return &V2SimulatorStep<Sim>; }
    else { return nullptr; }
}
template <typename Sim> constexpr DigitalCurling_PluginV2AreStopped V2SimulatorStoppedEntry() noexcept {
    if constexpr (std::is_base_of_v<digitalcurling::simulators::IStepSimulator, Sim>) { return &V2SimulatorStopped<Sim>; }
    else { return nullptr; }
}
template <typename Sim> constexpr DigitalCurling_PluginV2GetFloat V2SimulatorSecondsEntry() noexcept {
    if constexpr (std::is_base_of_v<digitalcurling::simulators::IStepSimulator, Sim>) { return &V2SimulatorSeconds<Sim>; }
    else { return nullptr; }
}
template <typename Sim, auto CalculateShot>
DigitalCurling_PluginV2Status DIGITALCURLING_PLUGIN_V2_CALL V2SimulatorCalculateShot(void* sim, const DigitalCurling_PluginV2Vector2* target, float speed, float angular_velocity, DigitalCurling_PluginV2Shot* out, char** error) noexcept {
    if constexpr (CalculateShot == nullptr) {
        return V2ReturnError(DIGITALCURLING_ERR_UNSUPPORTED_OPERATION, "CalculateShot: operation is not supported by this plugin.", error);
    } else {
        try { auto* box=V2Box(sim, V2BoxKind::kSimulator); if (!box) return V2BadHandle(sim, "CalculateShot", error); return static_cast<DigitalCurling_PluginV2Status>(SimulatorCalculateShotImpl<Sim, CalculateShot>(box->simulator, reinterpret_cast<const DigitalCurling_Vector2*>(target), speed, angular_velocity, reinterpret_cast<DigitalCurling_Shot*>(out), error)); }
        catch (...) { return V2ReturnError(DIGITALCURLING_ERR_UNKNOWN, "CalculateShot: unknown exception.", error); }
    }
}

} // namespace digitalcurling::plugins::detail
