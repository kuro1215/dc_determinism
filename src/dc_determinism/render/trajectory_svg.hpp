// `render trajectory`: the stone tracks of a captured shot, the collisions inferred from them and the SVG plot.
//
// Everything here works on values only (no I/O): `RenderSource` is built from a `CapturedTrajectory`, `infer_collisions`
// derives the collisions from the positions and velocities, `render_shot_svg` returns the SVG text. The output is
// deterministic: numbers are written with a fixed precision and every ordering is fixed.
//
// Presence: a stone is present in a frame unless all six values are within 1e-7 of zero (as in the worker; NaN keeps a
// stone present). A frame is usable for a stone when it is present and its position and velocity are finite.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "capture/dctr.hpp"
#include "json/json.hpp"
#include "logs/log_compare.hpp"

namespace dc_determinism {

extern const char* const kRenderSchema;  // "dc-determinism-render-v1"

// The per-frame values of one stone (float32 widened to double), one entry per frame.
struct StoneTrack {
    int id = 0;
    std::vector<char> present;
    std::vector<char> usable;
    std::vector<double> x, y, vx, vy;
};

struct RenderSource {
    std::int64_t frames = 0;
    std::vector<StoneTrack> stones;  // ascending stone id
};

RenderSource make_render_source(const CapturedTrajectory& trajectory);

// The recorded game-log trajectory as a source (`boards[0]` = the start board, then one board per logged frame):
// the stones present in any board, positions only (velocities 0, so no collision is inferred from it).
RenderSource make_log_render_source(const std::vector<Board>& boards);

// A collision inferred from the positions. Speeds are in m/s from the recorded velocity fields; a missing value
// (a frame without usable data) is empty.
struct Collision {
    std::int64_t step = 0;  // the frame in which the stones are first within the contact distance
    int collider = 0;       // the stone with the larger speed in the frame before (ties: the lower id)
    int target = 0;
    double x = 0.0, y = 0.0;  // midpoint of the two centres at `step`
    std::optional<double> collider_speed;
    std::optional<double> relative_speed;
    std::optional<double> target_speed_after;
};

constexpr double kStoneRadius = 0.145;  // digitalcurling/stone.hpp kRadius
constexpr double kDefaultContactMargin = 0.005;

// `margin` (metres, >= 0) is added to the stone diameter for the contact test and is the hysteresis of a pair.
std::vector<Collision> infer_collisions(const RenderSource& source, double margin);

// One plotted source of a shot (A, B, or the recorded log).
struct RenderedSource {
    bool log = false;   // the recorded game log: thin line on top, grey final circles, no labels or collisions
    std::string tag;    // "A" / "B" / "log"
    std::string label;  // "A: fcv1 msvc"; for the log the whole legend text
    std::string color;  // "#rrggbb"
    std::string status;
    std::int64_t steps = 0;
    RenderSource data;
    std::vector<Collision> collisions;
};

// The SVG text of one shot (sources in plot order).
std::string render_shot_svg(const std::string& shot_id, const std::vector<RenderedSource>& sources);

// The collision entries of `render.json` (`label` is `<tag><number>`).
Json collisions_json(const std::string& tag, const std::vector<Collision>& collisions);

// Text for an XML document: `& < > " '` escaped, code points XML 1.0 forbids (C0 controls other than tab, LF and CR,
// U+FFFE, U+FFFF) and invalid UTF-8 replaced with U+FFFD.
std::string xml_escape(const std::string& text);

}  // namespace dc_determinism
