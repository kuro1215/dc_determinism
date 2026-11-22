// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include "digitalcurling/plugins/handle_id.hpp"
#include "digitalcurling/plugins/plugin_api.hpp"
#include "digitalcurling/plugins/plugin_error.hpp"

#include "digitalcurling/plugins/loader_types.h"
#include "digitalcurling/plugins/detail/plugin_instance_list.hpp"

namespace digitalcurling::plugins::detail {


// --- IsCStruct ---
template<typename T> struct IsCStruct : std::false_type {};
template<> struct IsCStruct<DigitalCurling_Vector2> : std::true_type {};
template<> struct IsCStruct<DigitalCurling_Shot> : std::true_type {};
template<> struct IsCStruct<DigitalCurling_StoneState> : std::true_type {};
template<> struct IsCStruct<DigitalCurling_StoneCoordinate> : std::true_type {};


// --- CTypeConverter ---
template<typename CppT, typename CT> struct CTypeConverter;

// --- CTypeConverter (Readonly) ---
template <typename T>
struct CTypeConverter<std::nullptr_t, T*> {
    static constexpr bool needs_resolver = false;

    static const T* ToCType(std::nullptr_t) { return nullptr; }
};

//--- CTypeConverter (Readonly) Specializations ---
template<typename CHandleT>
struct CTypeConverter<HandleId, CHandleT*> {
    static constexpr bool needs_resolver = true;

    static std::shared_ptr<CHandleT> ToCType(PluginInstanceList& map, const HandleId& id) {
        return map.Get<CHandleT>(id);
    }
};

// --- CTypeConverter (Read/Write) ---
template<typename T>
struct CTypeConverter<T, T> {
    static constexpr bool needs_resolver = false;

    static const T ToCType(const T& value) { return value; }
    static const T FromCType(const T& c_value) { return c_value; }
};
template<typename T>
struct CTypeConverter<T*, T*> {
    static constexpr bool needs_resolver = false;

    static const T* ToCType(const T* value) { return value; }
    static const T* FromCType(const T* c_value) { return c_value; }
};
template<typename CppT, typename CT>
struct CTypeConverter<std::optional<CppT>, CT> {
    static constexpr bool needs_resolver = false;

    static const std::optional<CT> ToCType(const std::optional<CppT>& value) {
        if (!value.has_value()) return std::nullopt;
        return CTypeConverter<CppT, CT>::ToCType(*value);
    }
    static const std::optional<CppT> FromCType(const CT& c_value) {
        return CTypeConverter<CppT, CT>::FromCType(c_value);
    }
};
template<typename CppT, typename CT>
struct CTypeConverter<std::optional<CppT>, CT*> {
    static constexpr bool needs_resolver = false;

    static const std::optional<CT> ToCType(const std::optional<CppT>& value) {
        if (!value.has_value()) return std::nullopt;
        return CTypeConverter<CppT, CT>::ToCType(*value);
    }
    static const std::optional<CppT> FromCType(const CT* c_value) {
        if (!c_value) return std::nullopt;
        return CTypeConverter<CppT, CT>::FromCType(*c_value);
    }
};

// --- CTypeConverter (Read/Write) Enum Specializations ---
template<>
struct CTypeConverter<players::Gender, DigitalCurling_Gender> {
    static constexpr bool needs_resolver = false;

    static const DigitalCurling_Gender ToCType(const players::Gender& value) {
        return static_cast<DigitalCurling_Gender>(value);
    }
    static const players::Gender FromCType(const DigitalCurling_Gender& c_value) {
        return static_cast<players::Gender>(c_value);
    }
};

// --- CTypeConverter (Read/Write) C-struct Specializations ---
template<>
struct CTypeConverter<Vector2, DigitalCurling_Vector2> {
    static constexpr bool needs_resolver = false;

    static const DigitalCurling_Vector2 ToCType(const Vector2& value) {
        return DigitalCurling_Vector2 { value.x, value.y };
    }
    static const Vector2 FromCType(const DigitalCurling_Vector2& c_value) {
        return Vector2 { c_value.x, c_value.y };
    }
};
template<>
struct CTypeConverter<moves::Shot, DigitalCurling_Shot> {
    static constexpr bool needs_resolver = false;

    static const DigitalCurling_Shot ToCType(const moves::Shot& value) {
        return DigitalCurling_Shot {
            value.translational_velocity,
            value.angular_velocity,
            value.release_angle
        };
    }
    static const moves::Shot FromCType(const DigitalCurling_Shot& c_value) {
        return moves::Shot(c_value.translational_velocity, c_value.angular_velocity, c_value.release_angle);
    }
};
template<>
struct CTypeConverter<simulators::ISimulator::StoneState, DigitalCurling_StoneState> {
    static constexpr bool needs_resolver = false;

    static const DigitalCurling_StoneState ToCType(const simulators::ISimulator::StoneState& value) {
        return DigitalCurling_StoneState {
            CTypeConverter<Vector2, DigitalCurling_Vector2>::ToCType(value.position),
            value.angle,
            CTypeConverter<Vector2, DigitalCurling_Vector2>::ToCType(value.translational_velocity),
            value.angular_velocity
        };
    }
    static const simulators::ISimulator::StoneState FromCType(const DigitalCurling_StoneState& c_value) {
        return simulators::ISimulator::StoneState {
            CTypeConverter<Vector2, DigitalCurling_Vector2>::FromCType(c_value.position),
            c_value.angle,
            CTypeConverter<Vector2, DigitalCurling_Vector2>::FromCType(c_value.translational_velocity),
            c_value.angular_velocity
        };
    }
};
template<>
struct CTypeConverter<simulators::ISimulator::AllStones, DigitalCurling_StoneCoordinate> {
    static constexpr bool needs_resolver = false;

    static const DigitalCurling_StoneCoordinate ToCType(const simulators::ISimulator::AllStones& value) {
        auto c_coord = DigitalCurling_StoneCoordinate{};
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i].has_value()) {
                c_coord.stones[i] = CTypeConverter<simulators::ISimulator::StoneState, DigitalCurling_StoneState>::ToCType(*value[i]);
            }
        }
        return c_coord;
    }
    static const simulators::ISimulator::AllStones FromCType(const DigitalCurling_StoneCoordinate& c_value) {
        simulators::ISimulator::AllStones stones{};
        for (size_t i = 0; i < stones.size(); ++i) {
            if (c_value.stones[i].angle != 0.f ||
                c_value.stones[i].position.x != 0.f ||
                c_value.stones[i].position.y != 0.f ||
                c_value.stones[i].translational_velocity.x != 0.f ||
                c_value.stones[i].translational_velocity.y != 0.f ||
                c_value.stones[i].angular_velocity != 0.f) {
                stones[i] = CTypeConverter<simulators::ISimulator::StoneState, DigitalCurling_StoneState>::FromCType(c_value.stones[i]);
            }
        }
        return stones;
    }
};

// --- CTypeConverter (Read/Write) Specializations ---
template<>
struct CTypeConverter<std::string, char*> {
    static constexpr bool needs_resolver = false;

    static const char* ToCType(const std::string& value) {
        return value.c_str();
    }
    static const std::string FromCType(const char* c_value) {
        return c_value ? std::string(c_value) : std::string();
    }
};
template<>
struct CTypeConverter<std::string, const char*> : CTypeConverter<std::string, char*> {};

} // namespace digitalcurling::plugins::detail
