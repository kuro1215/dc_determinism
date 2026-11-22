// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

/// @file
/// @brief Define the HandleId class

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <random>
#include <stdexcept>

namespace digitalcurling::plugins {

/// @brief Opaque 64-bit ID that identifies a plugin object or a snapshot held by the loader
/// @details
/// Bits 63..48 are a random prefix drawn once per module, and bits 47..0 are a counter that starts at 1.
/// The value 0 is never issued and means "invalid".
///
/// An ID is never reused within a module, which is guaranteed by the counter: a stale ID cannot alias
/// an object created later. When the counter space (2^48 - 1 IDs) is exhausted, Generate throws
/// instead of reusing a value.
///
/// The counter and the prefix live in each module (executable or shared library) that instantiates Generate.
/// The random prefix only makes an ID issued by another loader module or process miss with probability
/// 1 - 2^-16 instead of aliasing. It is not a security boundary: IDs are neither secret nor unguessable.
class HandleId {
public:
    /// @brief Create an invalid ID (0). It is never returned by Generate.
    HandleId() = default;

    /// @brief Generate a new ID that is distinct from every ID generated before in this module
    /// @returns New ID
    /// @throws std::overflow_error The counter space is exhausted
    static HandleId Generate();

    /// @brief Create an ID from a raw value. Any value is accepted; an unknown ID is simply not found.
    /// @param[in] value Raw value of the ID
    /// @returns ID
    static HandleId FromValue(std::uint64_t value) {
        HandleId id;
        id.value_ = value;
        return id;
    }

    /// @brief Get the raw value of the ID
    /// @returns Raw value of the ID; 0 for an invalid ID
    std::uint64_t value() const { return value_; }

    /// @brief Compare two IDs
    friend bool operator==(const HandleId& lhs, const HandleId& rhs) { return lhs.value_ == rhs.value_; }
    /// @brief Compare two IDs
    friend bool operator!=(const HandleId& lhs, const HandleId& rhs) { return !(lhs == rhs); }

private:
    std::uint64_t value_ = 0;
};

namespace detail {

/// @brief Issues IDs made of a fixed 16-bit prefix and an incrementing 48-bit counter
class HandleIdGenerator {
public:
    /// @brief Number of bits of the counter
    static constexpr int kCounterBits = 48;
    /// @brief Largest counter value
    static constexpr std::uint64_t kMaxCounter = (std::uint64_t{1} << kCounterBits) - 1;

    /// @brief Create a generator
    /// @param[in] prefix Prefix of the issued IDs; only the lower 16 bits are used
    /// @param[in] first_counter Counter value of the first ID. 0 is not a valid counter value.
    HandleIdGenerator(std::uint16_t prefix, std::uint64_t first_counter = 1)
        : prefix_(static_cast<std::uint64_t>(prefix) << kCounterBits)
        , next_counter_(first_counter == 0 ? 1 : first_counter)
    {}

    HandleIdGenerator(const HandleIdGenerator&) = delete;
    HandleIdGenerator& operator=(const HandleIdGenerator&) = delete;

    /// @brief Issue a new ID
    /// @returns New ID
    /// @throws std::overflow_error The counter space is exhausted. Every later call throws as well.
    HandleId Generate() {
        // The counter is never rerolled, because that could reissue an earlier ID.
        const std::uint64_t counter = next_counter_.fetch_add(1, std::memory_order_relaxed);
        if (counter > kMaxCounter) {
            throw std::overflow_error("handle id space exhausted");
        }
        return HandleId::FromValue(prefix_ | counter);
    }

    /// @brief Get the generator used by HandleId::Generate
    /// @returns Generator of this module, with a prefix drawn once from std::random_device
    static HandleIdGenerator& GetDefault() {
        static HandleIdGenerator generator{[]() {
            std::random_device device;
            return static_cast<std::uint16_t>(device() & 0xFFFFu);
        }()};
        return generator;
    }

private:
    const std::uint64_t prefix_;
    std::atomic<std::uint64_t> next_counter_;
};

} // namespace detail

inline HandleId HandleId::Generate() {
    return detail::HandleIdGenerator::GetDefault().Generate();
}

} // namespace digitalcurling::plugins

/// @cond Doxygen_Suppress
template <>
struct std::hash<digitalcurling::plugins::HandleId> {
    std::size_t operator()(const digitalcurling::plugins::HandleId& id) const noexcept {
        // splitmix64 finalizer, so the constant prefix does not cluster the hash
        std::uint64_t x = id.value() + 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        x ^= x >> 31;
        return static_cast<std::size_t>(x);
    }
};
/// @endcond
