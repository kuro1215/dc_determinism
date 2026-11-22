// Deterministic synthetic boards shared by the fake C API worker (tool_fake_worker), the fake pybind worker
// (tool_fake_pybind) and the pybind tests. Not part of the tool.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace fakeboard {

inline double f32(double value) { return static_cast<double>(static_cast<float>(value)); }

struct Point {
    bool present = false;
    double x = 0.0;
    double y = 0.0;
};

// (x, y) of a stone at a simulator step, or absent; float32-exact.
inline Point stone(int position, int step, int sid) {
    if ((sid + position) % 3 == 0) return Point();
    Point point;
    point.present = true;
    point.x = f32(0.1 * sid + 0.01 * position + step * 1e-4);
    point.y = f32(1.0 + 0.2 * sid - step * 2e-4);
    return point;
}

using Board = std::array<Point, 16>;

inline Board board(int position, int step) {
    Board out;
    for (int sid = 0; sid < 16; ++sid) out[static_cast<std::size_t>(sid)] = stone(position, step, sid);
    return out;
}

inline void put32(std::string& out, std::uint32_t value) {
    for (int b = 0; b < 4; ++b) out.push_back(static_cast<char>((value >> (8 * b)) & 0xFF));
}

inline void put_float(std::string& out, double value) {
    float narrow = static_cast<float>(value);
    std::uint32_t bits;
    std::memcpy(&bits, &narrow, sizeof(bits));
    put32(out, bits);
}

// The 130-byte board of the worker's `O` answer: u16 mask, then x, y of the 16 slots as float32.
inline std::string encode(const Board& value) {
    std::uint16_t mask = 0;
    std::string coords;
    for (std::size_t sid = 0; sid < 16; ++sid) {
        if (value[sid].present) {
            mask = static_cast<std::uint16_t>(mask | (1u << sid));
            put_float(coords, value[sid].x);
            put_float(coords, value[sid].y);
        } else {
            put_float(coords, 0.0);
            put_float(coords, 0.0);
        }
    }
    std::string out;
    out.push_back(static_cast<char>(mask & 0xFF));
    out.push_back(static_cast<char>((mask >> 8) & 0xFF));
    return out + coords;
}

inline std::string base64(const std::string& data) {
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        std::uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8) |
                          static_cast<unsigned char>(data[i + 2]);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (i + 1 == data.size()) {
        std::uint32_t v = static_cast<unsigned char>(data[i]) << 16;
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        std::uint32_t v = (static_cast<unsigned char>(data[i]) << 16) | (static_cast<unsigned char>(data[i + 1]) << 8);
        out += alphabet[(v >> 18) & 63];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Six float32-exact values of a stone at a step; `present` false when it is absent.
struct State {
    bool present = false;
    double values[6] = {0, 0, 0, 0, 0, 0};
};

inline State trajectory_state(int position, int step, int sid, int steps) {
    Point point = stone(position, step, sid);
    State out;
    if (!point.present) return out;
    int remaining = steps - step;
    out.present = true;
    out.values[0] = point.x;
    out.values[1] = point.y;
    out.values[2] = f32(0.001 * step * (sid + 1));
    out.values[3] = f32(1e-3 * remaining);
    out.values[4] = f32(-2e-3 * remaining);
    out.values[5] = f32(1e-4 * remaining);
    return out;
}

// The worker capture file of a synthetic shot with steps + 1 frames (u32 mask, u32 frames, then the present
// stones' six values per frame).
inline std::string raw_trajectory(int position, int steps) {
    std::vector<int> stones;
    std::uint32_t mask = 0;
    for (int sid = 0; sid < 16; ++sid) {
        if (stone(position, 0, sid).present) {
            stones.push_back(sid);
            mask |= 1u << sid;
        }
    }
    std::string raw;
    put32(raw, mask);
    put32(raw, static_cast<std::uint32_t>(steps + 1));
    for (int step = 0; step <= steps; ++step) {
        for (int sid : stones) {
            State state = trajectory_state(position, step, sid, steps);
            for (double value : state.values) put_float(raw, value);
        }
    }
    return raw;
}

// A 16-slot board as a pybind final board (absent stones are [0, 0]): [[8 points], [8 points]].
inline std::vector<std::array<double, 2>> to_final_points(const Board& value) {
    std::vector<std::array<double, 2>> points;
    for (const Point& point : value) points.push_back({point.present ? point.x : 0.0, point.present ? point.y : 0.0});
    return points;
}

// The final board the fake pybind engines report: py27 equals Path A's board, py36 moves a stone of odd positions
// by 2 cm and drops the last stone of position 2.
inline Board pybind_board(const std::string& name, int position) {
    Board value = board(position, 100 + position);
    if (name == "fcv1-py36") {
        int first = -1, last = -1;
        for (int sid = 0; sid < 16; ++sid) {
            if (value[static_cast<std::size_t>(sid)].present) {
                if (first < 0) first = sid;
                last = sid;
            }
        }
        if (position % 2 == 1) value[static_cast<std::size_t>(first)].x = f32(value[static_cast<std::size_t>(first)].x + 0.02);
        if (position == 2) value[static_cast<std::size_t>(last)] = Point();
    }
    return value;
}

}  // namespace fakeboard
