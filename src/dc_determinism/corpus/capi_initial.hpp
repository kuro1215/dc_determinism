// `build_capi_initial(log, storage_position_only=True)`:
// the initial StoneCoordinate (16 stones x 6 float32, little endian) of a shot.
#pragma once

#include <array>
#include <cstdint>

#include "json/json.hpp"

namespace dc_determinism {

constexpr std::size_t kStoneCount = 16;
constexpr std::size_t kCoordSize = 384;  // 16 * 6 * 4

using Coordinate = std::array<std::uint8_t, kCoordSize>;

struct CapiInitial {
    Coordinate coord;
    int delivered_sid = 0;
};

// Doubles are computed as in Python and rounded to float32 on storage (ctypes c_float).
// `log` must hold at least end, shot, actual_move, simulator_storage.stones and
// trajectory.frames. Throws DcDeterminismError where Python raises.
CapiInitial build_capi_initial(const Json& log, bool storage_position_only = true);

// `delivered(log)`: (team, index, sid) of the first stone with a value in the old trajectory,
// else derived from the shot number.
struct Delivered {
    int team = 0;
    std::int64_t index = 0;
    std::int64_t sid = 0;
};
Delivered delivered(const Json& log);

// The input of the historical pybind engines for one old log (the legacy pybind input): the stone positions of the 16
// slots (x, y interleaved, doubles in shot coordinates; an absent stone is (0, 0); all zero for the first shot of an
// end), the shot counters, the delivery velocity and the pybind angular velocity. The pybind convention reverses the
// rotation sign of the old logs: ccw gives -pi/2, cw +pi/2. Throws DcDeterminismError where Python raises.
struct LegacyPybindInput {
    double positions[2 * kStoneCount] = {};
    std::int64_t total_shot_number = 0;
    std::int64_t shot_per_team = 0;
    double velocity_x = 0.0;
    double velocity_y = 0.0;
    double angular_velocity = 0.0;
    int team_number = 0;
};
LegacyPybindInput legacy_pybind_input(const Json& log);

}  // namespace dc_determinism
