#include "render/trajectory_svg.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

#include "sim/metrics.hpp"

namespace dc_determinism {

const char* const kRenderSchema = "dc-determinism-render-v1";

namespace {

constexpr double kViewMinY = 31.0;
constexpr double kViewMaxY = 41.0;
constexpr double kSheetHalfWidth = 2.375;
constexpr double kMovedThreshold = 0.05;  // metres from the first position
constexpr int kCanvasWidth = 980;
constexpr int kCanvasHeight = 820;
constexpr int kCanvasMargin = 96;
constexpr std::size_t kPanelRows = 8;  // collisions listed per source in the side panel

double widen(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<double>(value);
}

double distance_of(double ax, double ay, double bx, double by) {
    double dx = ax - bx;
    double dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

bool in_view(double y) { return y >= kViewMinY && y <= kViewMaxY; }

std::string fixed(double value, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    std::string text = buffer;
    // "-0.0" and friends: one spelling of zero keeps the files stable.
    bool zero = true;
    for (char c : text) {
        if (c != '-' && c != '0' && c != '.') zero = false;
    }
    if (zero && !text.empty() && text[0] == '-') text.erase(0, 1);
    return text;
}

std::string pixels(double value) { return fixed(value, 1); }

std::string speed_text(const std::optional<double>& value) { return value ? fixed(*value, 2) : std::string("n/a"); }

std::optional<double> finite_value(double value) {
    if (std::isfinite(value)) return value;
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------- collisions

std::optional<double> speed_at(const StoneTrack& stone, std::int64_t frame) {
    std::size_t i = static_cast<std::size_t>(frame);
    if (!stone.usable[i]) return std::nullopt;
    return finite_value(std::sqrt(stone.vx[i] * stone.vx[i] + stone.vy[i] * stone.vy[i]));
}

std::optional<double> speed_after_contact(const StoneTrack& a, const StoneTrack& b, const StoneTrack& target,
                                          std::int64_t step, std::int64_t frames, double contact) {
    for (std::int64_t u = step + 1; u < frames; ++u) {
        std::size_t i = static_cast<std::size_t>(u);
        if (!a.present[i] || !b.present[i] || !a.usable[i] || !b.usable[i]) continue;
        if (distance_of(a.x[i], a.y[i], b.x[i], b.y[i]) > contact) return speed_at(target, u);
    }
    for (std::int64_t u = frames - 1; u >= step; --u) {
        if (target.present[static_cast<std::size_t>(u)] && target.usable[static_cast<std::size_t>(u)]) {
            return speed_at(target, u);
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------- xml text

std::string replacement_character() { return "\xEF\xBF\xBD"; }

// The length of the valid UTF-8 sequence at `i` (code point in `cp`), or 0.
std::size_t decode_utf8(const std::string& text, std::size_t i, std::uint32_t& cp) {
    unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
        cp = lead;
        return 1;
    }
    std::size_t length = 0;
    std::uint32_t minimum = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        cp = lead & 0x1Fu;
        minimum = 0x80;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        cp = lead & 0x0Fu;
        minimum = 0x800;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        cp = lead & 0x07u;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        unsigned char next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0u) != 0x80u) return 0;
        cp = (cp << 6) | (next & 0x3Fu);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return length;
}

bool xml_forbidden(std::uint32_t cp) {
    if (cp < 0x20) return cp != 0x09 && cp != 0x0A && cp != 0x0D;
    return cp == 0xFFFE || cp == 0xFFFF;
}

// ---------------------------------------------------------------------------------------------- the plot

struct Box {
    double x0, y0, x1, y1;
};

bool overlaps(const Box& a, const Box& b) { return !(a.x1 < b.x0 || a.x0 > b.x1 || a.y1 < b.y0 || a.y0 > b.y1); }

double overlap_area(const Box& a, const Box& b) {
    if (!overlaps(a, b)) return 0.0;
    return std::max(0.0, std::min(a.x1, b.x1) - std::max(a.x0, b.x0)) *
           std::max(0.0, std::min(a.y1, b.y1) - std::max(a.y0, b.y0));
}

double point_box_distance(double px, double py, const Box& box) {
    double dx = std::max(std::max(box.x0 - px, 0.0), px - box.x1);
    double dy = std::max(std::max(box.y0 - py, 0.0), py - box.y1);
    return std::sqrt(dx * dx + dy * dy);
}

struct Point {
    double x, y;
};

// Line style of a source: the engines thick and translucent, the recorded log thin and opaque (drawn on top).
std::string stroke_style(const RenderedSource& source) {
    return source.log ? "stroke-width=\"1.15\" stroke-opacity=\"1.00\"" : "stroke-width=\"5.4\" stroke-opacity=\"0.30\"";
}

struct Segment {
    std::vector<Point> points;
};

struct StoneView {
    int id = 0;
    bool moved = false;
    std::vector<Segment> segments;  // the part of the path inside the view
    std::optional<Point> initial;   // frame 0 (usable, inside the view)
    std::optional<Point> final_at;  // last frame (usable, inside the view)
};

// The tracks of one source reduced to what the plot draws.
// `log`: a stone missing from the start board is the delivered stone even when it first appears inside the view.
std::vector<StoneView> stone_views(const RenderSource& source, bool log) {
    std::vector<StoneView> views;
    for (const StoneTrack& track : source.stones) {
        StoneView view;
        view.id = track.id;
        bool have_first = false;
        Point first{0.0, 0.0};
        double farthest = 0.0;
        for (std::int64_t f = 0; f < source.frames; ++f) {
            std::size_t i = static_cast<std::size_t>(f);
            if (!track.present[i] || !track.usable[i]) continue;
            if (!have_first) {
                have_first = true;
                first = Point{track.x[i], track.y[i]};
            }
            farthest = std::max(farthest, distance_of(track.x[i], track.y[i], first.x, first.y));
        }
        if (!have_first) continue;
        bool delivered = first.y < kViewMinY || (log && !track.present[0]);  // comes from outside the view
        view.moved = delivered || farthest >= kMovedThreshold;
        if (source.frames > 0) {
            if (track.present[0] && track.usable[0] && in_view(track.y[0])) view.initial = Point{track.x[0], track.y[0]};
            std::size_t last = static_cast<std::size_t>(source.frames - 1);
            if (track.present[last] && track.usable[last] && in_view(track.y[last])) {
                view.final_at = Point{track.x[last], track.y[last]};
            }
        }
        if (view.moved) {
            Segment current;
            for (std::int64_t f = 0; f < source.frames; ++f) {
                std::size_t i = static_cast<std::size_t>(f);
                bool drawn = track.present[i] && track.usable[i] && in_view(track.y[i]);
                if (drawn) {
                    current.points.push_back(Point{track.x[i], track.y[i]});
                } else if (!current.points.empty()) {
                    view.segments.push_back(std::move(current));
                    current = Segment();
                }
            }
            if (!current.points.empty()) view.segments.push_back(std::move(current));
        }
        views.push_back(std::move(view));
    }
    return views;
}

class Plot {
public:
    Plot(const std::string& shot_id, const std::vector<RenderedSource>& sources) : shot_id_(shot_id), sources_(sources) {}

    std::string render();

private:
    double sx(double x) const { return off_x_ + (x - min_x_) * scale_; }
    double sy(double y) const { return off_y_ + plot_h_ - (y - kViewMinY) * scale_; }

    void layout();
    void add_hard_box(Box box, double padding = 0.0) {
        hard_.push_back(Box{box.x0 - padding, box.y0 - padding, box.x1 + padding, box.y1 + padding});
    }
    void add_soft_box(const Box& box) { soft_.push_back(box); }
    std::string add_label(const std::string& text, double x, double y, double anchor_radius);
    void draw_sheet();
    void draw_header();
    void draw_paths();
    void draw_stones();
    void draw_labels();
    void draw_collisions();
    void draw_panel();
    std::string line(double x1, double y1, double x2, double y2, const char* css) const;

    const std::string& shot_id_;
    const std::vector<RenderedSource>& sources_;
    std::vector<std::vector<StoneView>> views_;
    std::vector<std::string> body_;
    std::vector<Box> hard_, soft_;
    std::vector<std::pair<std::string, Point>> label_requests_;  // text, circle centre in pixels
    double min_x_ = 0, max_x_ = 0, scale_ = 1, plot_w_ = 0, plot_h_ = 0, off_x_ = 0, off_y_ = 0;
    double stone_radius_ = 4.0;
};

std::string Plot::line(double x1, double y1, double x2, double y2, const char* css) const {
    return "<line class=\"" + std::string(css) + "\" x1=\"" + pixels(x1) + "\" y1=\"" + pixels(y1) + "\" x2=\"" +
           pixels(x2) + "\" y2=\"" + pixels(y2) + "\"/>";
}

void Plot::layout() {
    std::vector<double> xs = {-kSheetHalfWidth, kSheetHalfWidth};
    for (const std::vector<StoneView>& views : views_) {
        for (const StoneView& view : views) {
            for (const Segment& segment : view.segments) {
                for (const Point& point : segment.points) xs.push_back(point.x);
            }
            if (view.moved && view.initial) xs.push_back(view.initial->x);
            if (view.final_at) xs.push_back(view.final_at->x);
        }
    }
    min_x_ = *std::min_element(xs.begin(), xs.end()) - 0.03;
    max_x_ = *std::max_element(xs.begin(), xs.end()) + 0.03;
    scale_ = std::min((kCanvasWidth - kCanvasMargin * 2) / (max_x_ - min_x_),
                      (kCanvasHeight - kCanvasMargin * 2) / (kViewMaxY - kViewMinY));
    plot_w_ = (max_x_ - min_x_) * scale_;
    plot_h_ = (kViewMaxY - kViewMinY) * scale_;
    off_x_ = (kCanvasWidth - plot_w_) / 2;
    off_y_ = (kCanvasHeight - plot_h_) / 2;
    stone_radius_ = std::max(4.0, kStoneRadius * scale_);
}

void Plot::draw_sheet() {
    double top = sy(kViewMaxY);
    double bottom = sy(kViewMinY);
    body_.push_back(line(sx(-kSheetHalfWidth), bottom, sx(-kSheetHalfWidth), top, "sheet"));
    body_.push_back(line(sx(kSheetHalfWidth), bottom, sx(kSheetHalfWidth), top, "sheet"));
    for (double y : {40.234, 38.405, 32.004}) body_.push_back(line(sx(min_x_), sy(y), sx(max_x_), sy(y), "sheet"));
    body_.push_back(line(sx(0), bottom, sx(0), top, "sheet"));
    for (double r : {1.829, 1.219, 0.610, 0.152}) {
        body_.push_back("<circle class=\"sheet\" cx=\"" + pixels(sx(0)) + "\" cy=\"" + pixels(sy(38.405)) + "\" r=\"" +
                        pixels(r * scale_) + "\"/>");
    }
    for (int y : {32, 34, 36, 38, 40}) {
        double tx = sx(min_x_) - 42;
        double ty = sy(y) + 4;
        body_.push_back("<text x=\"" + pixels(tx) + "\" y=\"" + pixels(ty) + "\" class=\"small\">y=" +
                        std::to_string(y) + "</text>");
        add_hard_box(Box{tx - 2, ty - 11, tx + 30, ty + 3}, 3.0);
    }
}

void Plot::draw_header() {
    // Title and legend are centred on the canvas. The legend rows share one left edge (swatches aligned);
    // the block width comes from the same text-width estimate as the labels.
    double centre = kCanvasWidth / 2.0;
    body_.push_back("<text x=\"" + pixels(centre) + "\" y=\"22\" class=\"overlay-title\" text-anchor=\"middle\">shot " +
                    xml_escape(shot_id_) + "</text>");
    std::vector<std::string> texts;
    double block_width = 0.0;
    for (const RenderedSource& source : sources_) {
        std::string text = source.log ? source.label
                                      : source.label + "  " + source.status + "  steps=" + std::to_string(source.steps) +
                                            "  collisions=" + std::to_string(source.collisions.size());
        std::size_t code_points = 0;
        for (unsigned char c : text) code_points += (c & 0xC0) != 0x80 ? 1 : 0;
        block_width = std::max(block_width, 52.0 + 6.2 * static_cast<double>(code_points));
        texts.push_back(std::move(text));
    }
    double left = std::max(14.0, centre - block_width / 2.0);
    for (std::size_t k = 0; k < sources_.size(); ++k) {
        double y = 42.0 + 16.0 * static_cast<double>(k);
        body_.push_back("<line x1=\"" + pixels(left) + "\" y1=\"" + pixels(y - 4) + "\" x2=\"" + pixels(left + 44) +
                        "\" y2=\"" + pixels(y - 4) + "\" stroke=\"" + sources_[k].color + "\" " +
                        stroke_style(sources_[k]) + "/>");
        body_.push_back("<text x=\"" + pixels(left + 52) + "\" y=\"" + pixels(y) + "\" class=\"small\">" +
                        xml_escape(texts[k]) + "</text>");
    }
}

void Plot::draw_paths() {
    body_.push_back("<g clip-path=\"url(#plot-area)\">");
    for (std::size_t k = 0; k < sources_.size(); ++k) {
        for (const StoneView& view : views_[k]) {
            for (const Segment& segment : view.segments) {
                std::string points;
                std::string previous;
                std::size_t count = 0;
                std::vector<Point> screen;
                for (const Point& point : segment.points) {
                    std::string pair = pixels(sx(point.x)) + "," + pixels(sy(point.y));
                    if (pair == previous) continue;  // the same pixel: drop it (file size)
                    previous = pair;
                    if (!points.empty()) points += " ";
                    points += pair;
                    screen.push_back(Point{sx(point.x), sy(point.y)});
                    ++count;
                }
                if (count < 2) continue;
                body_.push_back("<polyline class=\"line\" points=\"" + points + "\" stroke=\"" + sources_[k].color + "\" " +
                                stroke_style(sources_[k]) + "/>");
                double inflate = sources_[k].log ? 2.5 : 3.7;
                for (std::size_t p = 1; p < screen.size(); ++p) {
                    add_soft_box(Box{std::min(screen[p - 1].x, screen[p].x) - inflate,
                                     std::min(screen[p - 1].y, screen[p].y) - inflate,
                                     std::max(screen[p - 1].x, screen[p].x) + inflate,
                                     std::max(screen[p - 1].y, screen[p].y) + inflate});
                }
            }
        }
    }
    body_.push_back("</g>");
}

void Plot::draw_stones() {
    std::set<std::string> drawn_initial;
    for (std::size_t k = 0; k < sources_.size(); ++k) {
        if (sources_[k].log) continue;  // the engines' frame 0 is the start; the log adds no initial circles
        for (const StoneView& view : views_[k]) {
            if (view.moved && view.initial) {
                double cx = sx(view.initial->x);
                double cy = sy(view.initial->y);
                std::string key = std::to_string(view.id) + ":" + pixels(cx) + "," + pixels(cy);
                if (drawn_initial.insert(key).second) {
                    body_.push_back("<circle class=\"contact\" cx=\"" + pixels(cx) + "\" cy=\"" + pixels(cy) + "\" r=\"" +
                                    pixels(stone_radius_) +
                                    "\" fill=\"#ffffff\" fill-opacity=\"0.35\" stroke=\"#333333\" stroke-opacity=\"0.85\"/>");
                    add_hard_box(Box{cx - stone_radius_, cy - stone_radius_, cx + stone_radius_, cy + stone_radius_});
                }
            }
        }
    }
    struct Labelled {
        int id;
        double x, y;
    };
    std::vector<Labelled> labelled;
    for (std::size_t k = 0; k < sources_.size(); ++k) {
        if (sources_[k].log) {
            // The recorded final positions: grey outlines on top, no labels of their own.
            for (const StoneView& view : views_[k]) {
                if (!view.final_at) continue;
                double cx = sx(view.final_at->x);
                double cy = sy(view.final_at->y);
                body_.push_back("<circle cx=\"" + pixels(cx) + "\" cy=\"" + pixels(cy) + "\" r=\"" +
                                pixels(stone_radius_) + "\" fill=\"" + sources_[k].color + "\" fill-opacity=\"0.08\" stroke=\"" +
                                sources_[k].color + "\" stroke-opacity=\"1.00\" stroke-width=\"1.3\"/>");
                add_hard_box(Box{cx - stone_radius_, cy - stone_radius_, cx + stone_radius_, cy + stone_radius_});
            }
            continue;
        }
        double radius = stone_radius_ * (1.0 - 0.15 * static_cast<double>(k));
        for (const StoneView& view : views_[k]) {
            if (!view.final_at) continue;
            double cx = sx(view.final_at->x);
            double cy = sy(view.final_at->y);
            body_.push_back("<circle class=\"stone\" cx=\"" + pixels(cx) + "\" cy=\"" + pixels(cy) + "\" r=\"" +
                            pixels(radius) + "\" fill=\"" + sources_[k].color + "\" fill-opacity=\"0.15\" stroke=\"" +
                            sources_[k].color + "\" stroke-opacity=\"0.85\"/>");
            add_hard_box(Box{cx - radius, cy - radius, cx + radius, cy + radius});
            // One label per stone and place: B repeats A's only where the stone ended elsewhere (4 px or more).
            bool near = false;
            for (const Labelled& other : labelled) {
                if (other.id == view.id && distance_of(other.x, other.y, cx, cy) < 4.0) near = true;
            }
            if (near) continue;
            labelled.push_back(Labelled{view.id, cx, cy});
            label_requests_.emplace_back("sid " + std::to_string(view.id), Point{cx, cy});
        }
    }
}

std::string Plot::add_label(const std::string& text, double x, double y, double anchor_radius) {
    double width_est = std::max(28.0, static_cast<double>(text.size()) * 6.2);
    double height_est = 13.0;
    std::vector<Point> candidates;
    for (double gap : {3.0, 5.0, 8.0, 12.0, 18.0, 26.0}) {
        double left = x - anchor_radius - gap - width_est;
        double right = x + anchor_radius + gap;
        double center_x = x - width_est / 2;
        double top_baseline = y - anchor_radius - gap;
        double bottom_baseline = y + anchor_radius + gap + height_est;
        double middle_baseline = y + 4;
        for (const Point& candidate : {Point{right, middle_baseline}, Point{left, middle_baseline},
                                       Point{center_x, top_baseline}, Point{center_x, bottom_baseline},
                                       Point{right, top_baseline}, Point{right, bottom_baseline},
                                       Point{left, top_baseline}, Point{left, bottom_baseline}}) {
            candidates.push_back(candidate);
        }
    }
    bool have_best = false;
    double best_score = 0.0;
    Point best{0.0, 0.0};
    Box best_box{0, 0, 0, 0};
    for (const Point& candidate : candidates) {
        Box box{candidate.x - 2, candidate.y - height_est + 2, candidate.x + width_est + 2, candidate.y + 3};
        bool hard_hit = false;
        double hard_overlap = 0.0;
        for (const Box& other : hard_) {
            if (overlaps(box, other)) hard_hit = true;
            hard_overlap += overlap_area(box, other);
        }
        double soft_overlap = 0.0;
        for (const Box& other : soft_) soft_overlap += overlap_area(box, other);
        double edge_distance = point_box_distance(x, y, box);
        bool in_bounds = 0 <= box.x0 && box.x1 <= kCanvasWidth && 0 <= box.y0 && box.y1 <= kCanvasHeight;
        double score = (in_bounds ? 0.0 : 100000.0) + (hard_hit ? 10000.0 : 0.0) + hard_overlap * 120.0 +
                       soft_overlap * 0.25 + edge_distance * 4.0 + std::max(0.0, edge_distance - 10.0) * 45.0;
        if (!have_best || score < best_score) {
            have_best = true;
            best_score = score;
            best = candidate;
            best_box = box;
        }
    }
    add_hard_box(best_box);
    return "<text x=\"" + pixels(best.x) + "\" y=\"" + pixels(best.y) + "\" class=\"small\">" + xml_escape(text) +
           "</text>";
}

void Plot::draw_labels() {
    for (const std::pair<std::string, Point>& request : label_requests_) {
        body_.push_back(add_label(request.first, request.second.x, request.second.y, stone_radius_));
    }
}

void Plot::draw_collisions() {
    for (std::size_t k = 0; k < sources_.size(); ++k) {
        const RenderedSource& source = sources_[k];
        // The marker sits beside the contact point (A up-left, B up-right) and a leader line leads to the point, so
        // that the markers of two sources and the stones stay readable.
        double dx = k == 0 ? -13.0 : 13.0;
        double dy = -13.0;
        for (std::size_t n = 0; n < source.collisions.size(); ++n) {
            const Collision& collision = source.collisions[n];
            if (!in_view(collision.y)) continue;  // stays in the panel and in render.json
            double px = sx(collision.x);
            double py = sy(collision.y);
            double cx = px + dx;
            double cy = py + dy;
            body_.push_back("<line x1=\"" + pixels(px) + "\" y1=\"" + pixels(py) + "\" x2=\"" + pixels(cx) + "\" y2=\"" +
                            pixels(cy) + "\" stroke=\"" + source.color + "\" stroke-width=\"1.2\"/>");
            body_.push_back("<circle cx=\"" + pixels(px) + "\" cy=\"" + pixels(py) + "\" r=\"2.5\" fill=\"" + source.color +
                            "\"/>");
            body_.push_back("<circle cx=\"" + pixels(cx) + "\" cy=\"" + pixels(cy) +
                            "\" r=\"8.0\" fill=\"#ffffff\" stroke=\"" + source.color + "\" stroke-width=\"1.6\"/>");
            body_.push_back("<text x=\"" + pixels(cx) + "\" y=\"" + pixels(cy + 3.2) +
                            "\" class=\"marker\" text-anchor=\"middle\">" + xml_escape(source.tag + std::to_string(n + 1)) +
                            "</text>");
        }
    }
}

std::vector<std::string> wrap_note(const std::string& text, std::size_t max_chars) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        std::string word = text.substr(start, end - start);
        start = end + 1;
        if (word.empty()) continue;
        std::string next = current.empty() ? word : current + " " + word;
        if (next.size() > max_chars && !current.empty()) {
            lines.push_back(current);
            current = word;
        } else {
            current = next;
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

void Plot::draw_panel() {
    bool any = false;
    for (const RenderedSource& source : sources_) any = any || !source.collisions.empty();
    if (!any) return;

    std::vector<std::pair<const char*, const char*>> notes = {
        {"step", "frame of the first contact (frame 0 = initial state)."},
        {"v", "speed of the collider stone before contact (m/s)."},
        {"rel", "relative speed of the two stones before contact (m/s)."},
        {"tgt", "speed of the target stone after the stones separated (m/s)."},
        {"", "Markers are numbered per source; no correspondence between A and B is implied."},
        {"", "Values are inferred from trajectories, not raw simulator collision events."},
    };
    bool has_log = false;
    for (const RenderedSource& source : sources_) has_log = has_log || source.log;
    if (has_log) notes.emplace_back("", "log: recorded positions, no collisions inferred.");
    std::vector<std::pair<std::string, std::vector<std::string>>> wrapped;
    std::size_t note_lines = 0;
    for (const std::pair<const char*, const char*>& note : notes) {
        wrapped.emplace_back(note.first, wrap_note(note.second, 50));
        note_lines += wrapped.back().second.size();
    }
    const double panel_w = 306.0, row_h = 14.0, note_line_h = 13.0;
    double panel_h = 12.0;
    for (const RenderedSource& source : sources_) {
        if (source.collisions.empty()) continue;
        std::size_t rows = std::min(source.collisions.size(), kPanelRows) + (source.collisions.size() > kPanelRows ? 1 : 0);
        panel_h += 20.0 + row_h * static_cast<double>(rows) + 6.0;
    }
    panel_h += 6.0 + note_line_h * static_cast<double>(note_lines) + 8.0;
    double panel_x = std::max(off_x_ + plot_w_ + 20.0, 0.0);
    if (panel_x + panel_w > kCanvasWidth - 4) panel_x = kCanvasWidth - panel_w - 4;
    double panel_y = std::min(std::max(off_y_ + 16, 16.0), kCanvasHeight - panel_h - 16);

    body_.push_back("<rect x=\"" + pixels(panel_x) + "\" y=\"" + pixels(panel_y) + "\" width=\"" + pixels(panel_w) +
                    "\" height=\"" + pixels(panel_h) +
                    "\" rx=\"6\" ry=\"6\" fill=\"#ffffff\" fill-opacity=\"0.92\" stroke=\"#777777\" stroke-opacity=\"0.60\"/>");
    double y = panel_y + 12.0;
    for (const RenderedSource& source : sources_) {
        if (source.collisions.empty()) continue;
        y += 14.0;
        body_.push_back("<rect x=\"" + pixels(panel_x + 10) + "\" y=\"" + pixels(y - 8) + "\" width=\"7\" height=\"7\" fill=\"" +
                        source.color + "\" fill-opacity=\"0.85\"/>");
        body_.push_back("<text x=\"" + pixels(panel_x + 22) + "\" y=\"" + pixels(y) + "\" class=\"overlay-title\">" +
                        xml_escape(source.label + " collisions: " + std::to_string(source.collisions.size())) + "</text>");
        y += 6.0;
        std::size_t shown = std::min(source.collisions.size(), kPanelRows);
        for (std::size_t n = 0; n < shown; ++n) {
            const Collision& c = source.collisions[n];
            y += row_h;
            std::string text = source.tag + std::to_string(n + 1) + " step=" + std::to_string(c.step) + " sid " +
                               std::to_string(c.collider) + "->" + std::to_string(c.target) +
                               " v=" + speed_text(c.collider_speed) + " rel=" + speed_text(c.relative_speed) +
                               " tgt=" + speed_text(c.target_speed_after);
            body_.push_back("<text x=\"" + pixels(panel_x + 10) + "\" y=\"" + pixels(y) + "\" class=\"overlay-text\">" +
                            xml_escape(text) + "</text>");
        }
        if (source.collisions.size() > shown) {
            y += row_h;
            body_.push_back("<text x=\"" + pixels(panel_x + 10) + "\" y=\"" + pixels(y) + "\" class=\"overlay-text\">... " +
                            std::to_string(source.collisions.size() - shown) + " more</text>");
        }
    }
    y += 6.0 + 4.0;
    for (const std::pair<std::string, std::vector<std::string>>& note : wrapped) {
        for (std::size_t n = 0; n < note.second.size(); ++n) {
            y += note_line_h;
            if (!note.first.empty() && n == 0) {
                body_.push_back("<text x=\"" + pixels(panel_x + 10) + "\" y=\"" + pixels(y) +
                                "\" class=\"overlay-text\" font-weight=\"700\">" + note.first + ":</text>");
            }
            body_.push_back("<text x=\"" + pixels(panel_x + (note.first.empty() ? 10.0 : 44.0)) + "\" y=\"" + pixels(y) +
                            "\" class=\"overlay-text\">" + xml_escape(note.second[n]) + "</text>");
        }
    }
}

std::string Plot::render() {
    for (const RenderedSource& source : sources_) views_.push_back(stone_views(source.data, source.log));
    layout();

    std::vector<std::string> head;
    head.push_back("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(kCanvasWidth) + "\" height=\"" +
                   std::to_string(kCanvasHeight) + "\" viewBox=\"0 0 " + std::to_string(kCanvasWidth) + " " +
                   std::to_string(kCanvasHeight) + "\">");
    head.push_back(
        "<style>text{font-family:Segoe UI,Arial,sans-serif;font-size:13px;fill:#222}.small{font-size:11px;fill:#555}"
        ".overlay-title{font-size:12px;font-weight:600;fill:#222}.overlay-text{font-size:11px;fill:#333}"
        ".marker{font-size:9px;font-weight:600;fill:#222}.sheet{fill:none;stroke:#e5e5e5;stroke-width:1}"
        ".line{fill:none;stroke-linecap:round;stroke-linejoin:round}.stone{stroke-width:1.6}"
        ".contact{stroke-width:1.6;stroke-dasharray:4 3}</style>");
    head.push_back("<defs><clipPath id=\"plot-area\"><rect x=\"" + pixels(sx(min_x_)) + "\" y=\"" + pixels(sy(kViewMaxY)) +
                   "\" width=\"" + pixels(plot_w_) + "\" height=\"" + pixels(plot_h_) + "\"/></clipPath></defs>");
    head.push_back("<rect width=\"100%\" height=\"100%\" fill=\"#fff\"/>");

    draw_sheet();
    draw_header();
    draw_paths();
    draw_stones();
    draw_labels();
    draw_collisions();
    draw_panel();

    std::string out;
    for (const std::string& piece : head) out += piece + "\n";
    for (const std::string& piece : body_) out += piece + "\n";
    out += "</svg>\n";
    return out;
}

Json optional_json(const std::optional<double>& value) {
    if (!value) return nullptr;
    return *value;
}

}  // namespace

std::string xml_escape(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        std::uint32_t cp = 0;
        std::size_t length = decode_utf8(text, i, cp);
        if (length == 0 || xml_forbidden(cp)) {
            out += replacement_character();
            i += length == 0 ? 1 : length;
            continue;
        }
        switch (cp) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out.append(text, i, length); break;
        }
        i += length;
    }
    return out;
}

RenderSource make_render_source(const CapturedTrajectory& trajectory) {
    RenderSource source;
    source.frames = trajectory.frames();
    std::vector<int> ids = trajectory.stones();
    std::sort(ids.begin(), ids.end());
    std::size_t frames = static_cast<std::size_t>(source.frames);
    for (int id : ids) {
        std::array<std::vector<std::uint32_t>, kDctrFieldCount> columns;
        for (std::size_t field = 0; field < kDctrFieldCount; ++field) {
            columns[field] = trajectory.column_bits(id, static_cast<int>(field));
        }
        StoneTrack track;
        track.id = id;
        track.present.assign(frames, 0);
        track.usable.assign(frames, 0);
        track.x.assign(frames, 0.0);
        track.y.assign(frames, 0.0);
        track.vx.assign(frames, 0.0);
        track.vy.assign(frames, 0.0);
        for (std::size_t f = 0; f < frames; ++f) {
            double values[kDctrFieldCount];
            for (std::size_t field = 0; field < kDctrFieldCount; ++field) values[field] = widen(columns[field][f]);
            track.present[f] = is_disabled(values, kDctrFieldCount) ? 0 : 1;
            track.x[f] = values[0];
            track.y[f] = values[1];
            track.vx[f] = values[3];
            track.vy[f] = values[4];
            track.usable[f] = track.present[f] && std::isfinite(values[0]) && std::isfinite(values[1]) &&
                                      std::isfinite(values[3]) && std::isfinite(values[4])
                                  ? 1
                                  : 0;
        }
        source.stones.push_back(std::move(track));
    }
    return source;
}

RenderSource make_log_render_source(const std::vector<Board>& boards) {
    RenderSource source;
    source.frames = static_cast<std::int64_t>(boards.size());
    std::size_t frames = boards.size();
    for (std::size_t id = 0; id < kBoardStones; ++id) {
        bool seen = false;
        for (const Board& board : boards) seen = seen || board[id].present;
        if (!seen) continue;
        StoneTrack track;
        track.id = static_cast<int>(id);
        track.present.assign(frames, 0);
        track.usable.assign(frames, 0);
        track.x.assign(frames, 0.0);
        track.y.assign(frames, 0.0);
        track.vx.assign(frames, 0.0);
        track.vy.assign(frames, 0.0);
        for (std::size_t f = 0; f < frames; ++f) {
            const Stone& stone = boards[f][id];
            track.present[f] = stone.present ? 1 : 0;
            track.x[f] = stone.x;
            track.y[f] = stone.y;
            track.usable[f] = stone.present && std::isfinite(stone.x) && std::isfinite(stone.y) ? 1 : 0;
        }
        source.stones.push_back(std::move(track));
    }
    return source;
}

std::vector<Collision> infer_collisions(const RenderSource& source, double margin) {
    const double contact = 2.0 * kStoneRadius + margin;
    const double release = contact + margin;
    std::vector<Collision> found;
    for (std::size_t first = 0; first < source.stones.size(); ++first) {
        for (std::size_t second = first + 1; second < source.stones.size(); ++second) {
            const StoneTrack& a = source.stones[first];
            const StoneTrack& b = source.stones[second];
            // `known`: the pair state is defined; it starts at the first frame in which both stones are present
            // (usable), where touching stones count as disarmed, and restarts after a stone was absent. A stone that
            // appears in contact is thus no collision either.
            bool known = false;
            bool armed = true;
            for (std::int64_t t = 0; t < source.frames; ++t) {
                std::size_t i = static_cast<std::size_t>(t);
                if (!a.present[i] || !b.present[i]) {
                    known = false;
                    continue;
                }
                if (!a.usable[i] || !b.usable[i]) continue;  // neither re-arms nor separates the pair
                double d = distance_of(a.x[i], a.y[i], b.x[i], b.y[i]);
                if (!known) {
                    known = true;
                    armed = d > contact;
                    continue;
                }
                if (!armed) {
                    if (d > release) armed = true;
                    continue;
                }
                if (d > contact) continue;
                armed = false;

                Collision hit;
                hit.step = t;
                hit.x = (a.x[i] + b.x[i]) / 2.0;
                hit.y = (a.y[i] + b.y[i]) / 2.0;
                std::optional<double> speed_a = speed_at(a, t - 1);
                std::optional<double> speed_b = speed_at(b, t - 1);
                bool b_collides = speed_b && (!speed_a || *speed_b > *speed_a);  // ties: the lower id (a)
                const StoneTrack& collider = b_collides ? b : a;
                const StoneTrack& target = b_collides ? a : b;
                hit.collider = collider.id;
                hit.target = target.id;
                hit.collider_speed = b_collides ? speed_b : speed_a;
                std::size_t before = static_cast<std::size_t>(t - 1);
                if (a.usable[before] && b.usable[before]) {
                    double dx = a.vx[before] - b.vx[before];
                    double dy = a.vy[before] - b.vy[before];
                    hit.relative_speed = finite_value(std::sqrt(dx * dx + dy * dy));
                }
                hit.target_speed_after = speed_after_contact(a, b, target, t, source.frames, contact);
                found.push_back(hit);
            }
        }
    }
    std::stable_sort(found.begin(), found.end(), [](const Collision& left, const Collision& right) {
        if (left.step != right.step) return left.step < right.step;
        int left_low = std::min(left.collider, left.target), right_low = std::min(right.collider, right.target);
        if (left_low != right_low) return left_low < right_low;
        return std::max(left.collider, left.target) < std::max(right.collider, right.target);
    });
    return found;
}

std::string render_shot_svg(const std::string& shot_id, const std::vector<RenderedSource>& sources) {
    Plot plot(shot_id, sources);
    return plot.render();
}

Json collisions_json(const std::string& tag, const std::vector<Collision>& collisions) {
    Json out = Json::array();
    for (std::size_t n = 0; n < collisions.size(); ++n) {
        const Collision& c = collisions[n];
        Json row = Json::object();
        row["label"] = tag + std::to_string(n + 1);
        row["step"] = c.step;
        row["stones"] = Json::array({c.collider, c.target});
        row["point"] = Json::array({c.x, c.y});
        row["collider_speed"] = optional_json(c.collider_speed);
        row["relative_speed"] = optional_json(c.relative_speed);
        row["target_speed_after"] = optional_json(c.target_speed_after);
        out.push_back(row);
    }
    return out;
}

}  // namespace dc_determinism
