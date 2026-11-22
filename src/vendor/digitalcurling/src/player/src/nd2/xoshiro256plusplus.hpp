// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace digitalcurling::players {

class Xoshiro256PlusPlus {
public:
    using result_type = std::uint64_t;

    explicit Xoshiro256PlusPlus(std::uint64_t seed = 0);

    static constexpr result_type min() { return 0; }
    static constexpr result_type max() { return std::numeric_limits<std::uint64_t>::max(); }

    result_type operator()();

    friend std::ostream& operator<<(std::ostream& os, const Xoshiro256PlusPlus& p);
    friend std::istream& operator>>(std::istream& is, Xoshiro256PlusPlus& p);

private:
    std::array<std::uint64_t, 4> states_;

    static constexpr uint64_t rotl(const uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
};

} // namespace digitalcurling::players
