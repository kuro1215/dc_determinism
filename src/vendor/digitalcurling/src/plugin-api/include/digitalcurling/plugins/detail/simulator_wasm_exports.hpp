// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines simulator WASM export macros for TypeScript

#pragma once

#include <cstddef>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <emscripten/bind.h>
#include <emscripten/val.h>

#include "digitalcurling/coordinate.hpp"
#include "digitalcurling/moves/shot.hpp"
#include "digitalcurling/simulators/i_simulator.hpp"
#include "digitalcurling/simulators/i_simulator_factory.hpp"
#include "digitalcurling/simulators/i_step_simulator.hpp"
#include "digitalcurling/plugins/detail/simulator_wasm_trajectory.hpp"

namespace digitalcurling::plugins::detail {

/// @cond Doxygen_Suppress
template <typename Factory>
class SimulatorWasmBridge {
public:
    static void Initialize(
        std::string const& factory_json,
        emscripten::val const& sheet_width = emscripten::val::undefined())
    {
        auto factory = std::make_unique<Factory>();
        if (!factory_json.empty()) {
            auto const json = nlohmann::json::parse(factory_json);
            from_json(json, *factory);
        }

        float width;
        if (!TryParseNumber(sheet_width, width))
            width = coordinate::kSheetWidthMax;
        else if (!coordinate::IsValidSheetWidth(width))
            throw std::invalid_argument("SimulatorWasmBridge.Initialize: sheet_width is out of range.");

        auto simulator = factory->CreateSimulator();
        if (!simulator)
            throw std::invalid_argument("SimulatorWasmBridge.Initialize: failed to create a simulator.");
        // A simulate-only simulator is accepted (e.g. for calculateShot); only its trajectory is unsupported.
        if (auto* step_simulator = dynamic_cast<simulators::IStepSimulator*>(simulator.get())) {
            auto const seconds = step_simulator->GetSecondsPerFrame();
            if (!std::isfinite(seconds) || seconds <= 0.f)
                throw std::invalid_argument("SimulatorWasmBridge.Initialize: invalid simulator time step.");
        }
        Context().factory = std::move(factory);
        Context().simulator = std::move(simulator);
        Context().sheet_width = width;
    }

    static emscripten::val Simulate(
        emscripten::val const& board,
        emscripten::val const& shot_info,
        emscripten::val const& steps_per_frame = emscripten::val::undefined())
    {
        if (!Context().simulator)
            return MakeTrajectoryResult(kErrorNotInitialized, {});

        try {
            float board_length;
            if (!TryParseNumber(board["length"], board_length) || board_length != kBoardValueCount)
                return MakeTrajectoryResult(kErrorInvalidBoard, {});

            auto const shot = ParseShotInfo(shot_info);
            if (!shot)
                return MakeTrajectoryResult(kErrorInvalidShot, {});

            int frame_interval = 1;
            if (!steps_per_frame.isUndefined()) {
                if (steps_per_frame.typeOf().as<std::string>() != "number" ||
                    !TryWasmFrameInterval(steps_per_frame.as<double>(), frame_interval)) {
                    return MakeTrajectoryResult(kErrorInvalidFramePerStep, {});
                }
            }

            auto stones = DecodeBoard(board, *shot);
            Context().simulator->SetStones(stones);

            auto result = SimulateWasmTrajectory(
                *Context().simulator, frame_interval, Context().sheet_width);
            return MakeTrajectoryResult(result.count, result.trajectory);
        } catch (...) {
            return MakeTrajectoryResult(kErrorSimulator, {});
        }
    }

    template <typename Simulator, moves::Shot (Simulator::*CalculateShotFunction)(Vector2 const&, float, float) const>
    static emscripten::val CalculateShot(
        emscripten::val const& target_position,
        emscripten::val const& target_speed,
        emscripten::val const& angular_velocity)
    {
        if (!Context().simulator)
            return MakeShotResult(kErrorNotInitialized, std::nullopt);

        try {
            float target_x, target_y, speed, angular_vel;
            if (!TryParseNumber(target_position["x"], target_x) ||
                !TryParseNumber(target_position["y"], target_y) ||
                !TryParseNumber(target_speed, speed) ||
                !TryParseNumber(angular_velocity, angular_vel))
            {
                return MakeShotResult(kErrorInvalidShot, std::nullopt);
            }

            auto* simulator = dynamic_cast<Simulator*>(Context().simulator.get());
            if (!simulator)
                return MakeShotResult(kErrorSimulator, std::nullopt);

            auto const shot = (simulator->*CalculateShotFunction)(Vector2(target_x, target_y), speed, angular_vel);
            return MakeShotResult(kErrorOk, shot);
        } catch (...) {
            return MakeShotResult(kErrorSimulator, std::nullopt);
        }
    }

    static void Dispose()
    {
        Context().simulator.reset();
        Context().factory.reset();
    }

private:
    static constexpr std::size_t kBoardValuesPerStone = 3;
    static constexpr std::size_t kBoardValueCount = kBoardValuesPerStone * StoneCoordinate::kStoneMax;

    static constexpr int kErrorOk = 0;
    static constexpr int kErrorNotInitialized = -1;
    static constexpr int kErrorInvalidBoard = -2;
    static constexpr int kErrorInvalidShot = -3;
    static constexpr int kErrorInvalidFramePerStep = -4;
    static constexpr int kErrorSimulator = -5;

    struct WasmContext {
        std::unique_ptr<Factory> factory;
        std::unique_ptr<simulators::ISimulator> simulator;
        float sheet_width = coordinate::kSheetWidthMax;
    };
    struct WasmShotInfo {
        StoneIndex index;
        moves::Shot shot;
    };

    static WasmContext& Context()
    {
        static WasmContext context;
        return context;
    }

    static bool TryToPositiveInt(float value, int& out_value)
    {
        if (std::isfinite(value) && value == std::floor(value) && value >= 0 &&
            static_cast<double>(value) <= static_cast<double>(std::numeric_limits<int>::max())) {
            out_value = static_cast<int>(value);
            return true;
        }
        return false;
    }
    static bool TryParseNumber(emscripten::val const& object, float& out_value)
    {
        if (object.isUndefined() || object.isNull()) return false;

        try {
            out_value = object.as<float>();
            return std::isfinite(out_value);
        } catch (...) {
            return false;
        }
    }
    static std::optional<WasmShotInfo> ParseShotInfo(emscripten::val const& shot_info)
    {
        if (shot_info.isUndefined() || shot_info.isNull())
            return std::nullopt;

        float index_float;
        if (!TryParseNumber(shot_info["index"], index_float) || index_float != std::floor(index_float))
            return std::nullopt;
        int index;
        if (!TryToPositiveInt(index_float, index) || index < 0 || index >= StoneCoordinate::kStoneMax)
            return std::nullopt;

        moves::Shot shot;
        if (!TryParseNumber(shot_info["translational_velocity"], shot.translational_velocity) ||
            !TryParseNumber(shot_info["angular_velocity"], shot.angular_velocity) ||
            !TryParseNumber(shot_info["release_angle"], shot.release_angle))
        {
            return std::nullopt;
        }

        return WasmShotInfo{ StoneIndex(index), shot };
    }

    static simulators::ISimulator::AllStones DecodeBoard(emscripten::val const& board, WasmShotInfo const& shot)
    {
        simulators::ISimulator::AllStones stones;
        auto const shot_index = static_cast<int>(shot.index.GetIndex());
        for (int i = 0; i < StoneCoordinate::kStoneMax; ++i) {
            auto const offset = static_cast<std::size_t>(i) * kBoardValuesPerStone;
            auto const x = board[offset].as<float>();
            auto const y = board[offset + 1].as<float>();
            auto const angle = board[offset + 2].as<float>();

            if (x == 0.f && y == 0.f && angle == 0.f && i != shot_index) {
                stones[i] = std::nullopt;
            } else {
                stones[i].emplace(Vector2(x, y), angle, Vector2(), 0.f);
            }
        }

        stones[shot_index].emplace(Vector2(), 0.f, shot.shot.ToVector2(), shot.shot.angular_velocity);
        return stones;
    }

    static emscripten::val MakeTrajectoryResult(int count, std::vector<float> const& trajectory)
    {
        if (trajectory.size() > kWasmMaxTrajectoryValues ||
            (count > 0 && static_cast<std::size_t>(count) != trajectory.size() / kWasmBoardValueCount) ||
            (count > 0 && trajectory.size() % kWasmBoardValueCount != 0) ||
            (count <= 0 && !trajectory.empty())) {
            throw std::length_error("SimulatorWasmBridge: invalid trajectory result.");
        }

        auto array = emscripten::val::global("Float32Array").new_(trajectory.size());
        if (!trajectory.empty()) {
            auto const source = emscripten::val(
                emscripten::typed_memory_view(trajectory.size(), trajectory.data())
            );
            array.call<void>("set", source);
        }

        auto result = emscripten::val::object();
        result.set("count", count);
        result.set("trajectory", array);
        return result;
    }

    static emscripten::val MakeShotResult(int error, std::optional<moves::Shot> const& shot)
    {
        auto result = emscripten::val::object();
        result.set("error", error);
        if (shot) {
            result.set("translational_velocity", shot->translational_velocity);
            result.set("angular_velocity", shot->angular_velocity);
            result.set("release_angle", shot->release_angle);
        }
        return result;
    }
};
/// @endcond

} // namespace digitalcurling::plugins::detail

#define DIGITALCURLING_EXPORT_SIMULATOR_WASM(FactoryClass) \
    EMSCRIPTEN_BINDINGS(digitalcurling_simulator_wasm) { \
        emscripten::function( \
            "init", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Initialize \
        ); \
        emscripten::function( \
            "simulate", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Simulate \
        ); \
        emscripten::function( \
            "dispose", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Dispose \
        ); \
    }

#define DIGITALCURLING_EXPORT_INVERTIBLE_SIMULATOR_WASM(FactoryClass, SimulatorClass, CalculateShotFunction) \
    EMSCRIPTEN_BINDINGS(digitalcurling_simulator_wasm) { \
        emscripten::function( \
            "init", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Initialize \
        ); \
        emscripten::function( \
            "simulate", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Simulate \
        ); \
        emscripten::function( \
            "calculateShot", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::template CalculateShot<SimulatorClass, CalculateShotFunction> \
        ); \
        emscripten::function( \
            "dispose", \
            &digitalcurling::plugins::detail::SimulatorWasmBridge<FactoryClass>::Dispose \
        ); \
    }
