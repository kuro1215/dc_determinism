#include "corpus/capi_initial.hpp"

#include <cmath>
#include <cstring>
#include <string>

#include "common/error.hpp"
#include "corpus/pytext.hpp"

namespace dc_determinism {

namespace {

constexpr double kHackYOnSimulation = 21.0315;
constexpr double kPi = 3.141592653589793;
constexpr std::int64_t kStonesPerTeam = 8;

[[noreturn]] void fail(const std::string& message) { throw DcDeterminismError("invalid shot log: " + message); }

const Json& require(const Json& object, const char* key) {
    if (!object.is_object() || !object.contains(key)) fail(std::string("missing '") + key + "'");
    return object[key];
}

// `dict.get(key, {})` that must yield a dict.
const Json& object_or_empty(const Json& parent, const char* key) {
    static const Json empty = Json::object();
    if (!parent.contains(key)) return empty;
    const Json& value = parent[key];
    if (!value.is_object()) fail(std::string("'") + key + "' is not an object");
    return value;
}

double number(const Json& value) {
    double out = 0.0;
    std::string reason;
    if (!py_float(value, out, reason)) fail(reason);
    return out;
}

// `float(obj.get(key, fallback))`
double number_or(const Json& object, const char* key, double fallback) {
    return object.contains(key) ? number(object[key]) : fallback;
}

std::int64_t integer(const Json& value) {
    std::int64_t out = 0;
    std::string reason;
    if (!py_int(value, out, reason)) fail(reason);
    return out;
}

void store_float(Coordinate& coord, std::size_t sid, std::size_t field, double value) {
    float narrow = static_cast<float>(value);
    std::uint32_t bits;
    std::memcpy(&bits, &narrow, sizeof(bits));
    std::uint8_t* out = coord.data() + (sid * 6 + field) * 4;
    out[0] = static_cast<std::uint8_t>(bits & 0xff);
    out[1] = static_cast<std::uint8_t>((bits >> 8) & 0xff);
    out[2] = static_cast<std::uint8_t>((bits >> 16) & 0xff);
    out[3] = static_cast<std::uint8_t>((bits >> 24) & 0xff);
}

void store_stone(Coordinate& coord, std::size_t sid, double x, double y, double angle, double vx, double vy,
                 double angular) {
    store_float(coord, sid, 0, x);
    store_float(coord, sid, 1, y);
    store_float(coord, sid, 2, angle);
    store_float(coord, sid, 3, vx);
    store_float(coord, sid, 4, vy);
    store_float(coord, sid, 5, angular);
}

std::int64_t floor_mod2(std::int64_t value) { return ((value % 2) + 2) % 2; }

std::int64_t floor_div2(std::int64_t value) {
    std::int64_t quotient = value / 2;
    if (value % 2 != 0 && value < 0) --quotient;
    return quotient;
}

}  // namespace

Delivered delivered(const Json& log) {
    if (log.contains("trajectory")) {
        const Json& trajectory = log["trajectory"];
        if (!trajectory.is_object()) fail("'trajectory' is not an object");
        if (trajectory.contains("frames")) {
            const Json& frames = trajectory["frames"];
            if (!frames.is_array()) fail("'frames' is not an array");
            for (const Json& frame : frames) {
                if (!frame.is_array()) fail("a trajectory frame is not an array");
                for (const Json& row : frame) {
                    if (row.is_null()) continue;
                    if (!row.is_object()) fail("a trajectory row is not an object");
                    if (!row.contains("value") || row["value"].is_null()) continue;
                    Delivered out;
                    const Json& team = require(row, "team");
                    out.team = team.is_string() && team.get<std::string>() == "team0" ? 0 : 1;
                    out.index = integer(require(row, "index"));
                    out.sid = out.team * kStonesPerTeam + out.index;
                    return out;
                }
            }
        }
    }
    std::int64_t shot = integer(require(log, "shot"));
    Delivered out;
    out.team = static_cast<int>(floor_mod2(shot));
    out.index = floor_div2(shot);
    out.sid = out.team * kStonesPerTeam + out.index;
    return out;
}

CapiInitial build_capi_initial(const Json& log, bool storage_position_only) {
    std::int64_t end = integer(require(log, "end"));
    std::int64_t shot = integer(require(log, "shot"));
    int side = static_cast<int>(floor_mod2(end));
    Delivered who = delivered(log);
    if (who.sid < 0 || who.sid >= static_cast<std::int64_t>(kStoneCount)) {
        fail("delivered stone index out of range");
    }

    CapiInitial out;
    out.coord.fill(0);
    out.delivered_sid = static_cast<int>(who.sid);

    if (shot != 0) {
        const Json& storage = object_or_empty(log, "simulator_storage");
        if (storage.contains("stones")) {
            const Json& stones = storage["stones"];
            if (!stones.is_array()) fail("'stones' is not an array");
            std::size_t sid = 0;
            for (const Json& stone : stones) {
                std::size_t current = sid++;
                if (stone.is_null()) continue;
                if (!stone.is_object()) fail("a stone is not an object");
                if (current >= kStoneCount) fail("stone index out of range");
                const Json& position = require(stone, "position");
                double px = number(require(position, "x"));
                double py = number(require(position, "y"));
                double x = side == 0 ? px : -px;
                double y = side == 0 ? py + kHackYOnSimulation : -py + kHackYOnSimulation;
                static const Json empty = Json::object();
                const Json* linear = &empty;
                if (stone.contains("linear_velocity")) {
                    linear = &stone["linear_velocity"];
                } else if (stone.contains("translational_velocity")) {
                    linear = &stone["translational_velocity"];
                }
                if (!linear->is_object()) fail("a stone velocity is not an object");
                double lx = number_or(*linear, "x", 0.0);
                double ly = number_or(*linear, "y", 0.0);
                double vx = side == 0 ? lx : -lx;
                double vy = side == 0 ? ly : -ly;
                bool position_only = storage_position_only && static_cast<std::int64_t>(current) != who.sid;
                double angle = 0.0;
                double angular = 0.0;
                if (!position_only) {
                    double raw_angle = number_or(stone, "angle", 0.0);
                    angle = side == 0 ? raw_angle : raw_angle + kPi;
                    angular = number_or(stone, "angular_velocity", 0.0);
                }
                store_stone(out.coord, current, x, y, angle, position_only ? 0.0 : vx, position_only ? 0.0 : vy,
                            angular);
            }
        }
    }

    const Json& move = object_or_empty(log, "actual_move");
    static const Json default_velocity = [] {
        Json v = Json::object();
        v["x"] = 0.0;
        v["y"] = 0.0;
        return v;
    }();
    const Json* velocity = &default_velocity;
    if (move.contains("velocity")) {
        velocity = &move["velocity"];
        if (!velocity->is_object()) fail("'velocity' is not an object");
    }
    std::string rotation = "cw";
    if (move.contains("rotation")) {
        if (!move["rotation"].is_string()) fail("'rotation' is not a string");
        rotation = move["rotation"].get<std::string>();
    }
    double vx = number_or(*velocity, "x", 0.0);
    double vy = number_or(*velocity, "y", 0.0);
    double angular = ascii_lower(rotation) == "ccw" ? kPi / 2.0 : -kPi / 2.0;
    store_stone(out.coord, static_cast<std::size_t>(who.sid), 0.0, 0.0, 0.0, vx, vy, angular);
    return out;
}

LegacyPybindInput legacy_pybind_input(const Json& log) {
    LegacyPybindInput out;
    std::int64_t shot = integer(require(log, "shot"));
    Delivered who = delivered(log);
    if (shot != 0) {
        std::int64_t end = integer(require(log, "end"));
        int side = static_cast<int>(floor_mod2(end));
        const Json& storage = object_or_empty(log, "simulator_storage");
        if (storage.contains("stones")) {
            const Json& stones = storage["stones"];
            if (!stones.is_array()) fail("'stones' is not an array");
            std::size_t sid = 0;
            for (const Json& stone : stones) {
                std::size_t current = sid++;
                if (stone.is_null()) continue;
                if (!stone.is_object()) fail("a stone is not an object");
                if (current >= kStoneCount) fail("stone index out of range");
                const Json& position = require(stone, "position");
                double px = number(require(position, "x"));
                double py = number(require(position, "y"));
                out.positions[2 * current] = side == 0 ? px : -px;
                out.positions[2 * current + 1] = side == 0 ? py + kHackYOnSimulation : -py + kHackYOnSimulation;
            }
        }
    }
    const Json& move = require(log, "actual_move");
    const Json& rotation = require(move, "rotation");
    if (!rotation.is_string()) fail("'rotation' is not a string");
    const Json& velocity = require(move, "velocity");
    out.velocity_x = number(require(velocity, "x"));
    out.velocity_y = number(require(velocity, "y"));
    out.angular_velocity = ascii_lower(rotation.get<std::string>()) == "ccw" ? -kPi / 2.0 : kPi / 2.0;
    out.total_shot_number = shot + 1;
    out.shot_per_team = who.index;
    out.team_number = who.team;
    return out;
}

}  // namespace dc_determinism
