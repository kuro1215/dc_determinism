// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Defines JSON conversion functions

#pragma once

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>

/// @cond Doxygen_Suppress

// Strict variant of NLOHMANN_JSON_SERIALIZE_ENUM.
// Unlike the original, which silently maps unknown values to the first entry,
// this throws std::invalid_argument in both directions when no entry matches.
#define DIGITALCURLING_JSON_SERIALIZE_ENUM_STRICT(ENUM_TYPE, ...)                                  \
    template <typename BasicJsonType>                                                              \
    inline void to_json(BasicJsonType & j, ENUM_TYPE const& e)                                     \
    {                                                                                              \
        static_assert(std::is_enum<ENUM_TYPE>::value, #ENUM_TYPE " must be an enum!");             \
        static std::pair<ENUM_TYPE, BasicJsonType> const m[] = __VA_ARGS__;                        \
        auto const it = std::find_if(std::begin(m), std::end(m),                                   \
            [e](std::pair<ENUM_TYPE, BasicJsonType> const& ej_pair) -> bool {                      \
                return ej_pair.first == e;                                                         \
            });                                                                                    \
        if (it == std::end(m))                                                                     \
            throw std::invalid_argument("invalid " #ENUM_TYPE " value");                           \
        j = it->second;                                                                            \
    }                                                                                              \
    template <typename BasicJsonType>                                                              \
    inline void from_json(BasicJsonType const& j, ENUM_TYPE & e)                                   \
    {                                                                                              \
        static_assert(std::is_enum<ENUM_TYPE>::value, #ENUM_TYPE " must be an enum!");             \
        static std::pair<ENUM_TYPE, BasicJsonType> const m[] = __VA_ARGS__;                        \
        auto const it = std::find_if(std::begin(m), std::end(m),                                   \
            [&j](std::pair<ENUM_TYPE, BasicJsonType> const& ej_pair) -> bool {                     \
                return ej_pair.second == j;                                                        \
            });                                                                                    \
        if (it == std::end(m))                                                                     \
            throw std::invalid_argument("invalid " #ENUM_TYPE " value");                           \
        e = it->first;                                                                             \
    }

/// @endcond

/// @cond Doxygen_Suppress
namespace {

// Helper class for the overloaded trick
// Reference: https://dev.to/tmr232/that-overloaded-trick-overloading-lambdas-in-c17
template<class... Ts> struct Overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> Overloaded(Ts...)->Overloaded<Ts...>;

template <typename T> struct type_identity { using type = T; };
template <typename T> using type_identity_t = typename type_identity<T>::type;

template<typename T>
inline T try_get_to(nlohmann::json const& j, std::string const& key, T &target, type_identity_t<T> default_value)
{
    if (j.contains(key)) {
        return j.at(key).get_to(target);
    } else {
        target = default_value;
        return default_value;
    }
}

} // unnamed namespace


namespace digitalcurling::json_util {

// Reads an integer that must be an exact JSON integer within the range of `Int`.
// Floats (even integral ones such as 1.0) and booleans are rejected instead of being truncated or converted.
// `name` is used for the error message only.
template <typename Int>
inline Int GetCheckedInteger(nlohmann::json const& value, char const* name)
{
    static_assert(std::is_integral<Int>::value && !std::is_same<Int, bool>::value, "Int must be an integer type.");

    auto const from_unsigned = [&](std::uint64_t number) -> std::optional<Int> {
        if (number <= static_cast<std::uint64_t>(std::numeric_limits<Int>::max()))
            return static_cast<Int>(number);
        return std::nullopt;
    };

    std::optional<Int> result;
    if (value.is_number_unsigned()) {
        result = from_unsigned(value.get<nlohmann::json::number_unsigned_t>());
    } else if (value.is_number_integer()) {
        auto const number = value.get<nlohmann::json::number_integer_t>();
        if (number >= 0) {
            result = from_unsigned(static_cast<std::uint64_t>(number));
        } else if (std::is_signed<Int>::value
            && number >= static_cast<nlohmann::json::number_integer_t>(std::numeric_limits<Int>::min())) {
            result = static_cast<Int>(number);
        }
    }

    if (!result.has_value()) {
        throw std::invalid_argument(
            std::string("invalid ") + name + ": must be an integer between "
            + std::to_string(static_cast<long long>(std::numeric_limits<Int>::min())) + " and "
            + std::to_string(static_cast<unsigned long long>(std::numeric_limits<Int>::max())) + ".");
    }
    return result.value();
}

// Reads the integer stored under `key`. See GetCheckedInteger.
template <typename Int>
inline Int GetChecked(nlohmann::json const& j, char const* key)
{
    return GetCheckedInteger<Int>(j.at(key), key);
}

// Reads a finite number that fits into a float.
// Values outside the range of float would otherwise silently become infinity.
inline float GetCheckedFloat(nlohmann::json const& value, char const* name)
{
    if (!value.is_number())
        throw std::invalid_argument(std::string("invalid ") + name + ": must be a number.");

    double const number = value.get<double>();
    if (!std::isfinite(number) || std::fabs(number) > static_cast<double>(std::numeric_limits<float>::max()))
        throw std::invalid_argument(std::string("invalid ") + name + ": must be a finite number representable as a float.");
    return static_cast<float>(number);
}

// Reads the float stored under `key`. See GetCheckedFloat.
inline float GetCheckedFloatAt(nlohmann::json const& j, char const* key)
{
    return GetCheckedFloat(j.at(key), key);
}

} // namespace digitalcurling::json_util


namespace nlohmann {

// std::optional
template <typename T>
struct adl_serializer<std::optional<T>> {
    static void to_json(json & j, std::optional<T> const& opt)
    {
        if (opt) {
            j = *opt;
        } else {
            j = nullptr;
        }
    }

    static void from_json(json const& j, std::optional<T> & opt)
    {
        if (j.is_null()) {
            opt = std::nullopt;
        } else {
            opt = j.get<T>();
        }
    }
};

// std::chrono::milliseconds
template <>
struct adl_serializer<std::chrono::milliseconds> {
    static void to_json(json & j, std::chrono::milliseconds const& ms)
    {
        j = static_cast<double>(ms.count()) / 1000.0;
    }

    static void from_json(json const& j, std::chrono::milliseconds & ms)
    {
        // Durations are finite and may be negative (remaining time after a time loss);
        // the cap keeps the millisecond count exact in a double.
        constexpr double kMaxMilliseconds = 9007199254740992.0;  // 2^53
        double const seconds = j.get<double>();
        double const milliseconds = seconds * 1000.0;
        if (!std::isfinite(seconds) || !(std::fabs(milliseconds) < kMaxMilliseconds))
            throw std::invalid_argument("invalid duration: must be a finite number of seconds that fits in milliseconds.");

        ms = std::chrono::milliseconds(
            static_cast<std::chrono::milliseconds::rep>(
                std::round(milliseconds)));
    }
};

} // namespace nlohmann
/// @endcond
