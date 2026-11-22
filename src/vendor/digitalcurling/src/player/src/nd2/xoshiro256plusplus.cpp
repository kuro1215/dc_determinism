// Copyright (c) 2022-2026 UEC Takeshi Ito Laboratory
// SPDX-License-Identifier: MIT

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include "xoshiro256plusplus.hpp"

namespace digitalcurling::players {

Xoshiro256PlusPlus::Xoshiro256PlusPlus(std::uint64_t seed) {
    // SplitMix64
    std::uint64_t x = seed, z = 0;
    for (int i = 0; i < 4; i++) {
        z = (x += 0x9e3779b97f4a7c15);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
        z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
        states_[i] = z ^ (z >> 31);
    }
}

Xoshiro256PlusPlus::result_type Xoshiro256PlusPlus::operator()() {
    const std::uint64_t result = rotl(states_[0] + states_[3], 23) + states_[0], t = states_[1] << 17;

    states_[2] ^= states_[0];
    states_[3] ^= states_[1];
    states_[1] ^= states_[2];
    states_[0] ^= states_[3];

    states_[2] ^= t;
    states_[3] = rotl(states_[3], 45);

    return result;
}

std::ostream& operator<<(std::ostream& os, const Xoshiro256PlusPlus& p) {
    static const char hex_chars[] = "0123456789abcdef";

    char buf[68];
    char* ptr = buf;
    for (int i = 0; i < 4; i++) {
        uint64_t v = p.states_[i];
        for (int j = 15; j >= 0; j--) {
            ptr[j] = hex_chars[v & 0xF];
            v >>= 4;
        }
        ptr[16] = ' ';
        ptr += 17;
    }
    return os.write(buf, 67);
}
std::istream& operator>>(std::istream& is, Xoshiro256PlusPlus& p) {
    std::ios_base::fmtflags f(is.flags());

    // Read into a temporary so that a failed read leaves `p` unchanged.
    std::array<std::uint64_t, 4> states{};
    bool ok = true;

    is >> std::hex;
    for (int i = 0; i < 4; i++) {
        if (!(is >> states[i])) {
            ok = false;
            break;
        }
    }

    is.flags(f);

    // The all-zero state is a fixed point of the generator and is never a valid state.
    if (ok && (states[0] | states[1] | states[2] | states[3]) == 0) {
        is.setstate(std::ios_base::failbit);
    } else if (ok) {
        p.states_ = states;
    }
    return is;
}

} // namespace digitalcurling::players
