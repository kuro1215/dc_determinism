// Exact results recorded at the base commit 2c7ebc9 (before `Simulate()` replaced the
// mode-based API) through `SimulatorSimulate(simulator, Full, 0, sheet_width)`.
// The scenarios are defined in `baseline_scenarios.hpp`; the order here is the same.
//
// fcv1 uses float physics with Box2D and libm math, so its results are tied to a
// toolchain. The recordings on MSVC 19.52 x64 and GCC 13.3 x86-64 (glibc 2.39) are bit-identical for
// these scenarios. fcv1plus uses fixed-point arithmetic and is the same on every platform.

#pragma once

#include <cstddef> // defines __GLIBC__ on glibc, used by the toolchain check below
#include "baseline_scenarios.hpp"

namespace digitalcurling::test {

// Only the toolchains the fcv1 set was recorded with: MSVC 19.52 x64 and GCC 13.3 with glibc 2.39 on x86-64 Linux.
// Other toolchains compare Simulate() with a step loop of the same build only.
#if (defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) && _MSC_VER == 1952) || \
    (defined(__GNUC__) && !defined(__clang__) && __GNUC__ == 13 && __GNUC_MINOR__ == 3 && \
     defined(__linux__) && defined(__x86_64__) && defined(__GLIBC__) && __GLIBC__ == 2 && __GLIBC_MINOR__ == 39)
#define DIGITALCURLING_TEST_HAVE_FCV1_BASELINE 1
#endif

#ifdef DIGITALCURLING_TEST_HAVE_FCV1_BASELINE
inline std::array<BaselineBits, 7> const kFcv1Baseline = {{
    // draw
    {{
        {true, {0xbfc9de47u, 0x42138950u, 0x4210be48u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // takeout
    {{
        {true, {0x3cb5f518u, 0x420eda20u, 0x3d1dabfcu, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {true, {0x3e99999au, 0x4218cccdu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0xbecccccdu, 0x421b999au, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // side_out
    {{
        {false, {}},
        {true, {0x3f000000u, 0x42180000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // back_out
    {{
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // cluster
    {{
        {true, {0xbf104961u, 0x421b306eu, 0xc02c110au, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3e947ae1u, 0x421a0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {true, {0xbea1adb8u, 0x42224334u, 0xc01ad3d7u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3d8e6ec0u, 0x421872b3u, 0xbdc08643u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3f19999au, 0x42180000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {true, {0xbdace85cu, 0x4216767fu, 0x4059719cu, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // narrow_sheet
    {{
        {false, {}},
        {true, {0xbfcccccdu, 0x41f00000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // invalid_width
    {{
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
}};
#endif

inline std::array<BaselineBits, 7> const kFcv1PlusBaseline = {{
    // draw
    {{
        {true, {0xbfc9c644u, 0x421389a2u, 0xbfc1b059u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // takeout
    {{
        {true, {0x3cb435c0u, 0x420ed7d2u, 0x3d19c150u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {true, {0x3e99999au, 0x4218cccdu, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0xbeccccceu, 0x421b999au, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // side_out
    {{
        {false, {}},
        {true, {0x3f000000u, 0x42180000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // back_out
    {{
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // cluster
    {{
        {true, {0xbf30f4aau, 0x421b41edu, 0x403eb92eu, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3e947ae2u, 0x421a0000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {true, {0xbe8bd05eu, 0x4221997fu, 0xc02bdc82u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3d8f4fa0u, 0x421872b9u, 0xbdbe15f8u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {true, {0x3f19999au, 0x42180000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {true, {0xbdabab28u, 0x42167505u, 0xc038a4f3u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // narrow_sheet
    {{
        {false, {}},
        {true, {0xbfcccccdu, 0x41f00000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
    // invalid_width
    {{
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
        {false, {}},
    }},
}};

} // namespace digitalcurling::test
