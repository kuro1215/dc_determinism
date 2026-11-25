// Unit tests of `render trajectory`: the collision inference, the SVG text and the command (synthetic archives,
// no worker).
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <set>

#include "capture/archive.hpp"
#include "capture/dctr.hpp"
#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "json/writer.hpp"
#include "render/trajectory_svg.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

using State = std::array<std::uint32_t, 6>;
using Frame = std::vector<State>;

std::uint32_t bits_of(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void put32(std::string& out, std::uint32_t value) {
    for (int b = 0; b < 4; ++b) out.push_back(static_cast<char>((value >> (8 * b)) & 0xFF));
}

std::string raw_of(std::uint32_t mask, const std::vector<Frame>& frames) {
    std::string raw;
    put32(raw, mask);
    put32(raw, static_cast<std::uint32_t>(frames.size()));
    for (const Frame& frame : frames) {
        for (const State& state : frame) {
            for (std::uint32_t value : state) put32(raw, value);
        }
    }
    return raw;
}

State state(float x, float y, float vx = 0.0f, float vy = 0.0f) {
    return {bits_of(x), bits_of(y), bits_of(0.0f), bits_of(vx), bits_of(vy), bits_of(0.0f)};
}

State absent() { return {0, 0, 0, 0, 0, 0}; }

// Stone 1 rests at (0, 35); stone 0 is `distance` above it.
Frame pair_frame(float distance, float vy0 = 0.0f, float vy1 = 0.0f) {
    return {state(0.0f, 35.0f + distance, 0.0f, vy0), state(0.0f, 35.0f, 0.0f, vy1)};
}

RenderSource source_of(const std::vector<Frame>& frames) {
    return make_render_source(CapturedTrajectory(raw_to_payload(raw_of(0x3u, frames), "s", 0, "fcv1")));
}

RenderSource distances(const std::vector<float>& values) {
    std::vector<Frame> frames;
    for (float value : values) frames.push_back(pair_frame(value));
    return source_of(frames);
}

std::vector<Collision> collisions_of(const std::vector<float>& values, double margin = kDefaultContactMargin) {
    return infer_collisions(distances(values), margin);
}

// ---------------------------------------------------------------------------------------- a small XML check

bool allowed_xml_text(const std::string& text, std::string& problem) {
    std::size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '&') {
            bool known = false;
            for (const char* entity : {"&amp;", "&lt;", "&gt;", "&quot;", "&apos;"}) {
                if (text.compare(i, std::strlen(entity), entity) == 0) known = true;
            }
            if (!known) {
                problem = "bare ampersand";
                return false;
            }
        }
        if (c < 0x20 && c != 0x09 && c != 0x0A && c != 0x0D) {
            problem = "forbidden control character";
            return false;
        }
        if (c == 0xEF && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xBF &&
            (static_cast<unsigned char>(text[i + 2]) == 0xBE || static_cast<unsigned char>(text[i + 2]) == 0xBF)) {
            problem = "U+FFFE or U+FFFF";
            return false;
        }
        ++i;
    }
    return true;
}

// Tags are balanced, attribute values are quoted and free of `<`, text has only known entities and no code point XML 1.0
// forbids.
bool well_formed(const std::string& xml, std::string& problem) {
    std::vector<std::string> stack;
    std::size_t i = 0;
    int roots = 0;
    while (i < xml.size()) {
        if (xml[i] != '<') {
            std::size_t end = xml.find('<', i);
            if (end == std::string::npos) end = xml.size();
            std::string text = xml.substr(i, end - i);
            if (!allowed_xml_text(text, problem)) return false;
            if (stack.empty() && text.find_first_not_of(" \n\r\t") != std::string::npos) {
                problem = "text outside the root";
                return false;
            }
            i = end;
            continue;
        }
        if (xml.compare(i, 2, "</") == 0) {
            std::size_t end = xml.find('>', i);
            if (end == std::string::npos) {
                problem = "unterminated end tag";
                return false;
            }
            std::string name = xml.substr(i + 2, end - i - 2);
            if (stack.empty() || stack.back() != name) {
                problem = "mismatched end tag " + name;
                return false;
            }
            stack.pop_back();
            i = end + 1;
            continue;
        }
        std::size_t j = i + 1;
        while (j < xml.size() && xml[j] != ' ' && xml[j] != '/' && xml[j] != '>') ++j;
        std::string name = xml.substr(i + 1, j - i - 1);
        if (name.empty()) {
            problem = "empty tag name";
            return false;
        }
        bool self_closing = false;
        for (;;) {
            if (j >= xml.size()) {
                problem = "unterminated tag " + name;
                return false;
            }
            if (xml[j] == '>') break;
            if (xml.compare(j, 2, "/>") == 0) {
                self_closing = true;
                ++j;
                break;
            }
            if (xml[j] == '"') {
                std::size_t close = xml.find('"', j + 1);
                if (close == std::string::npos) {
                    problem = "unterminated attribute value in " + name;
                    return false;
                }
                std::string value = xml.substr(j + 1, close - j - 1);
                if (value.find('<') != std::string::npos || !allowed_xml_text(value, problem)) {
                    if (problem.empty()) problem = "'<' in an attribute value";
                    return false;
                }
                j = close + 1;
                continue;
            }
            ++j;
        }
        if (stack.empty()) ++roots;
        if (!self_closing) stack.push_back(name);
        i = j + 1;
    }
    if (!stack.empty()) {
        problem = "unclosed tag " + stack.back();
        return false;
    }
    if (roots != 1) {
        problem = "not exactly one root element";
        return false;
    }
    return true;
}

std::size_t count_of(const std::string& text, const std::string& part) {
    std::size_t count = 0;
    for (std::size_t at = text.find(part); at != std::string::npos; at = text.find(part, at + part.size())) ++count;
    return count;
}

RenderedSource rendered(const std::string& tag, const std::string& color, const RenderSource& data, double margin = 0.005) {
    RenderedSource source;
    source.tag = tag;
    source.label = tag + ": fcv1 msvc";
    source.color = color;
    source.status = "stopped";
    source.steps = data.frames > 0 ? data.frames - 1 : 0;
    source.data = data;
    source.collisions = infer_collisions(data, margin);
    return source;
}

// Stone 0 slides towards the resting stone 1 and hits it in frame 14; stone 1 then moves on.
std::vector<Frame> head_on_frames() {
    std::vector<Frame> frames;
    for (int t = 0; t < 20; ++t) {
        float y0 = t <= 14 ? 37.0f - 0.125f * static_cast<float>(t) : 35.25f;
        float y1 = t <= 14 ? 35.0f : 35.0f - 0.125f * static_cast<float>(t - 14);
        frames.push_back({state(0.0f, y0, 0.0f, t <= 13 ? -1.0f : 0.0f), state(0.0f, y1, 0.0f, t <= 13 ? 0.0f : -0.5f)});
    }
    return frames;
}

}  // namespace

// ---------------------------------------------------------------------------------------- collision inference

TEST(head_on_hit_is_counted_once_at_the_right_step) {
    std::vector<Collision> found = infer_collisions(source_of(head_on_frames()), 0.005);
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    if (found.size() != 1) return;
    const Collision& hit = found[0];
    CHECK_EQ(hit.step, static_cast<std::int64_t>(14));
    CHECK_EQ(hit.collider, 0);
    CHECK_EQ(hit.target, 1);
    CHECK_EQ(hit.x, 0.0);
    CHECK_EQ(hit.y, 35.125);
    CHECK(hit.collider_speed && *hit.collider_speed == 1.0);
    CHECK(hit.relative_speed && *hit.relative_speed == 1.0);
    CHECK(hit.target_speed_after && *hit.target_speed_after == 0.5);
}

TEST(a_pair_touching_in_the_initial_state_is_not_a_collision) {
    CHECK_EQ(collisions_of({0.25f, 0.25f, 0.25f}).size(), static_cast<std::size_t>(0));
    // ... but a later hit after the stones separated is
    std::vector<Collision> found = collisions_of({0.25f, 0.25f, 0.25f, 0.5f, 0.25f});
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    if (!found.empty()) CHECK_EQ(found[0].step, static_cast<std::int64_t>(4));
}

TEST(separation_then_a_second_hit_counts_twice) {
    std::vector<Collision> found = collisions_of({1.0f, 0.5f, 0.25f, 0.25f, 0.5f, 1.0f, 0.5f, 0.25f});
    CHECK_EQ(found.size(), static_cast<std::size_t>(2));
    if (found.size() == 2) {
        CHECK_EQ(found[0].step, static_cast<std::int64_t>(2));
        CHECK_EQ(found[1].step, static_cast<std::int64_t>(7));
    }
}

TEST(jitter_around_the_contact_threshold_is_one_collision) {
    // the contact distance is 0.295 and the hysteresis releases the pair above 0.3
    CHECK_EQ(collisions_of({1.0f, 0.29f, 0.297f, 0.29f, 0.297f, 0.29f}).size(), static_cast<std::size_t>(1));
    CHECK_EQ(collisions_of({1.0f, 0.29f, 0.297f, 0.29f, 0.5f, 0.29f}).size(), static_cast<std::size_t>(2));
}

TEST(the_contact_margin_widens_the_contact_distance) {
    CHECK_EQ(collisions_of({1.0f, 0.32f}, 0.005).size(), static_cast<std::size_t>(0));
    CHECK_EQ(collisions_of({1.0f, 0.32f}, 0.05).size(), static_cast<std::size_t>(1));
}

TEST(a_removed_stone_rearms_the_pair_and_is_no_separation) {
    // hit at 1, stone 1 removed in 2 and 3, back at a distance in 4, hit again in 5
    std::vector<Frame> frames = {pair_frame(1.0f), pair_frame(0.25f), {state(0.0f, 35.25f), absent()},
                                 {state(0.0f, 35.25f), absent()}, pair_frame(0.5f), pair_frame(0.25f)};
    std::vector<Collision> found = infer_collisions(source_of(frames), 0.005);
    CHECK_EQ(found.size(), static_cast<std::size_t>(2));
    if (found.size() == 2) {
        CHECK_EQ(found[0].step, static_cast<std::int64_t>(1));
        CHECK_EQ(found[1].step, static_cast<std::int64_t>(5));
    }

    // the target never separates before it is removed: its speed at its last present frame is reported
    std::vector<Frame> removed = {pair_frame(1.0f), pair_frame(0.25f, 0.0f, -0.5f), {state(0.0f, 35.25f), absent()},
                                  {state(0.0f, 35.25f), absent()}};
    found = infer_collisions(source_of(removed), 0.005);
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    if (!found.empty()) CHECK(found[0].target_speed_after && *found[0].target_speed_after == 0.5);

    // a stone absent in the initial state is never "touching", and one that appears in contact is no collision
    std::vector<Frame> appears = {{state(0.0f, 35.25f), absent()}, {state(0.0f, 35.25f), absent()}, pair_frame(0.25f),
                                  pair_frame(0.25f)};
    CHECK_EQ(infer_collisions(source_of(appears), 0.005).size(), static_cast<std::size_t>(0));
}

TEST(frames_that_are_not_finite_neither_rearm_nor_separate) {
    float nan = std::numeric_limits<float>::quiet_NaN();
    std::vector<Frame> frames = {pair_frame(1.0f), pair_frame(0.25f), {state(nan, 35.25f), state(0.0f, 35.0f)},
                                 pair_frame(0.25f)};
    CHECK_EQ(infer_collisions(source_of(frames), 0.005).size(), static_cast<std::size_t>(1));

    // the frame before the hit has no usable velocity of stone 1: the missing values are empty, never NaN
    std::vector<Frame> before = {pair_frame(1.0f), {state(0.0f, 36.0f, 0.0f, -1.0f), state(0.0f, 35.0f, nan, 0.0f)},
                                 pair_frame(0.25f)};
    std::vector<Collision> found = infer_collisions(source_of(before), 0.005);
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    if (found.empty()) return;
    CHECK(found[0].collider_speed.has_value());
    CHECK(!found[0].relative_speed.has_value());
    Json listed = collisions_json("A", found);
    CHECK(listed[0]["relative_speed"].is_null());
    CHECK_EQ(listed[0]["label"].get<std::string>(), std::string("A1"));
    std::string dumped = json_dumps(listed, DumpOptions::compact(false, false));  // allow_nan=false would throw
    CHECK(dumped.find("NaN") == std::string::npos);
}

// ---------------------------------------------------------------------------------------- the SVG

TEST(xml_escape_replaces_what_xml_forbids) {
    std::string text = std::string("a&b<c>d\"e'f\tg\n") + '\0' + '\x01' + "\xEF\xBF\xBF" + "\xC3\xA9" + "\xFF" + "\xED\xA0\x80";
    std::string escaped = xml_escape(text);
    CHECK(escaped.find("a&amp;b&lt;c&gt;d&quot;e&apos;f\tg\n") == 0);
    CHECK_EQ(count_of(escaped, "\xEF\xBF\xBD"), static_cast<std::size_t>(2 + 1 + 1 + 3));  // NUL, U+0001, U+FFFF, 0xFF, surrogate (3 bytes)
    CHECK(escaped.find("\xC3\xA9") != std::string::npos);
    std::string problem;
    CHECK(allowed_xml_text(escaped, problem));
}

TEST(the_svg_is_well_formed_and_shows_the_collision) {
    RenderedSource source = rendered("A", "#00a86b", source_of(head_on_frames()));
    CHECK_EQ(source.collisions.size(), static_cast<std::size_t>(1));
    std::string svg = render_shot_svg("shot-1/end:0/shot:0", {source});
    std::string problem;
    CHECK(well_formed(svg, problem));
    if (!problem.empty()) ::testing::report_failure(__FILE__, __LINE__, "not well-formed: " + problem);
    CHECK(svg.compare(0, 4, "<svg") == 0);
    CHECK(svg.size() >= 7 && svg.compare(svg.size() - 7, 6, "</svg>") == 0);
    CHECK_EQ(count_of(svg, "class=\"marker\""), static_cast<std::size_t>(1));
    CHECK(svg.find(">A1</text>") != std::string::npos);
    CHECK(svg.find("inferred from trajectories") != std::string::npos);
    CHECK(svg.find("collisions=1") != std::string::npos);
    CHECK(svg.find("<polyline") != std::string::npos);
    CHECK(svg.find("<clipPath") != std::string::npos);
    CHECK(svg.find("A: fcv1 msvc") != std::string::npos);
    // a source without a collision draws no marker and no panel
    RenderedSource quiet = rendered("B", "#2563eb", distances({1.0f, 0.9f, 0.8f}));
    std::string plain = render_shot_svg("quiet", {quiet});
    CHECK(well_formed(plain, problem));
    CHECK_EQ(count_of(plain, "class=\"marker\""), static_cast<std::size_t>(0));
    CHECK(plain.find("inferred from trajectories") == std::string::npos);
    // both sources in one plot, their markers numbered independently
    std::string both = render_shot_svg("both", {source, rendered("B", "#2563eb", source_of(head_on_frames()))});
    CHECK(well_formed(both, problem));
    CHECK(both.find(">A1</text>") != std::string::npos);
    CHECK(both.find(">B1</text>") != std::string::npos);
    CHECK(both.find("#2563eb") != std::string::npos);
}

TEST(the_svg_text_from_archives_is_escaped) {
    std::string id = std::string("id&<>\"' ") + '\0' + '\x01' + "x";
    RenderedSource source = rendered("A", "#00a86b", source_of(head_on_frames()));
    source.label = "A: <engine & \"compiler\">";
    std::string svg = render_shot_svg(id, {source});
    std::string problem;
    CHECK(well_formed(svg, problem));
    if (!problem.empty()) ::testing::report_failure(__FILE__, __LINE__, "not well-formed: " + problem);
    CHECK(svg.find("id&amp;&lt;&gt;&quot;&apos; \xEF\xBF\xBD\xEF\xBF\xBDx") != std::string::npos);
    CHECK(svg.find("&lt;engine &amp; &quot;compiler&quot;&gt;") != std::string::npos);
}

TEST(an_empty_capture_still_gets_the_sheet) {
    std::string svg = render_shot_svg("empty", {rendered("A", "#00a86b", source_of({}))});
    std::string problem;
    CHECK(well_formed(svg, problem));
    CHECK(svg.find("class=\"sheet\"") != std::string::npos);
    CHECK(svg.find("shot empty") != std::string::npos);
    CHECK(svg.find("collisions=0") != std::string::npos);
}

TEST(the_svg_is_deterministic) {
    std::string first = render_shot_svg("s", {rendered("A", "#00a86b", source_of(head_on_frames()))});
    std::string second = render_shot_svg("s", {rendered("A", "#00a86b", source_of(head_on_frames()))});
    CHECK(first == second);
}

// ---------------------------------------------------------------------------------------- the command

namespace {

struct ShotSpec {
    std::string id;
    std::int64_t index;
    std::string raw;       // the worker capture file (or, when `payload` is set, ignored)
    std::string payload;   // a prebuilt member (an invalid one, for the failure test)
};

void write_archive(const std::filesystem::path& path, const std::string& engine, const std::string& compiler,
                   const std::vector<ShotSpec>& shots, const std::string& record_digest = std::string(64, 'r')) {
    CaptureWriter writer(path);
    Json rows = Json::array();
    std::set<std::int64_t> members;
    for (const ShotSpec& shot : shots) {
        if (members.insert(shot.index).second) {
            writer.add(capture_member_name(shot.index),
                       shot.payload.empty() ? raw_to_payload(shot.raw, shot.id, shot.index, engine) : shot.payload);
        }
        Json row = Json::object();
        row["index"] = shot.index;
        row["shot_id"] = shot.id;
        row["status"] = "stopped";
        row["steps"] = 19;
        row["member"] = capture_member_name(shot.index);
        rows.push_back(row);
    }
    Json contract = Json::object();
    contract["engine"] = engine;
    contract["observation"] = "raw-stonecoordinate-v1";
    contract["max_steps"] = 60000;
    contract["prepared_sha256"] = std::string(64, 'p');
    contract["record_digest"] = record_digest;
    contract["corpus_input_sha256"] = std::string(64, 'c');
    Json index = Json::object();
    index["schema"] = kCaptureSchema;
    index["format"] = kDctrFormat;
    index["contract"] = contract;
    if (!compiler.empty()) {
        Json loader = Json::object();
        loader["compiler"] = compiler;
        index["provenance"] = Json::object({{"loader", loader}});
    }
    index["shots"] = rows;
    writer.commit(index);
}

std::string quiet_raw() { return raw_of(0x3u, {pair_frame(1.0f), pair_frame(1.0f), pair_frame(1.0f)}); }

std::string twice_raw() {
    return raw_of(0x3u, {pair_frame(1.0f), pair_frame(0.25f), pair_frame(0.5f), pair_frame(1.0f), pair_frame(0.25f)});
}

int run_render(const std::vector<std::string>& arguments) {
    std::vector<std::string> line = {"render", "trajectory"};
    line.insert(line.end(), arguments.begin(), arguments.end());
    std::unique_ptr<cli::Parser> parser = cli::build_parser();
    return cli::dispatch(parser->parse(line));
}

bool throws(const std::vector<std::string>& arguments) {
    try {
        run_render(arguments);
    } catch (const DcDeterminismError&) {
        return true;
    }
    return false;
}

std::map<std::string, std::string> contents_of(const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory)) {
        files[path_to_utf8(entry.path().filename())] = read_file(entry.path());
    }
    return files;
}

std::string p(const std::filesystem::path& path) { return path_to_utf8(path); }

}  // namespace

TEST(render_trajectory_writes_the_svgs_and_render_json) {
    testing::TempDir dir;
    std::vector<ShotSpec> a_shots = {{"s0", 0, raw_of(0x3u, head_on_frames()), ""},
                                     {"s1", 1, quiet_raw(), ""},
                                     {"s2", 2, twice_raw(), ""}};
    std::vector<ShotSpec> b_shots = {{"s0", 0, raw_of(0x3u, head_on_frames()), ""},
                                     {"s1", 1, quiet_raw(), ""},
                                     {"s3", 3, quiet_raw(), ""}};
    write_archive(dir / "a.zip", "fcv1", "msvc", a_shots);
    write_archive(dir / "b.zip", "fcv1plus", "", b_shots, std::string(64, 'x'));

    CHECK_EQ(run_render({p(dir / "a.zip"), p(dir / "b.zip"), "--output", p(dir / "out1"), "--workers", "1"}), 0);
    std::map<std::string, std::string> first = contents_of(dir / "out1");
    CHECK_EQ(first.size(), static_cast<std::size_t>(3));  // 0.svg, 1.svg, render.json
    CHECK(first.count("0.svg") == 1 && first.count("1.svg") == 1 && first.count("render.json") == 1);
    CHECK(first.count("2.svg") == 0 && first.count("3.svg") == 0);
    std::string problem;
    for (const std::string& name : {std::string("0.svg"), std::string("1.svg")}) {
        CHECK(well_formed(first[name], problem));
    }
    CHECK(first["0.svg"].find("A: fcv1 msvc") != std::string::npos);
    CHECK(first["0.svg"].find("B: fcv1plus") != std::string::npos);

    Json report = read_json(dir / "out1" / "render.json");
    CHECK_EQ(report["schema"].get<std::string>(), std::string("dc-determinism-render-v1"));
    CHECK_EQ(report["contact_margin_m"].get<double>(), 0.005);
    CHECK_EQ(report["inputs"]["a"]["label"].get<std::string>(), std::string("A: fcv1 msvc"));
    CHECK(report["inputs"]["a"]["compiler"].is_string());
    CHECK(report["inputs"]["b"]["compiler"].is_null());
    CHECK_EQ(report["inputs"]["b"]["label"].get<std::string>(), std::string("B: fcv1plus"));
    CHECK_EQ(report["inputs"]["a"]["sha256"].get<std::string>(), sha256_file(dir / "a.zip"));
    CHECK_EQ(report["shots"].size(), static_cast<std::size_t>(2));
    CHECK_EQ(report["shots"][0]["shot_id"].get<std::string>(), std::string("s0"));
    CHECK_EQ(report["shots"][0]["file"].get<std::string>(), std::string("0.svg"));
    CHECK_EQ(report["shots"][0]["sources"]["a"]["collision_count"].get<int>(), 1);
    CHECK_EQ(report["shots"][0]["sources"]["b"]["collisions"][0]["step"].get<int>(), 14);
    CHECK_EQ(report["shots"][0]["sources"]["a"]["frames"].get<int>(), 20);
    CHECK_EQ(report["shots"][1]["sources"]["a"]["collision_count"].get<int>(), 0);
    CHECK_EQ(report["a_only"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(report["a_only"][0].get<std::string>(), std::string("s2"));
    CHECK_EQ(report["b_only"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(report["b_only"][0].get<std::string>(), std::string("s3"));
    CHECK_EQ(report["contract_differences"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(report["contract_differences"][0]["member"].get<std::string>(), std::string("record_digest"));

    // the output does not depend on the number of workers
    CHECK_EQ(run_render({p(dir / "a.zip"), p(dir / "b.zip"), "--output", p(dir / "out4") + "/", "--workers", "4"}), 0);
    CHECK(contents_of(dir / "out4") == first);
}

TEST(render_trajectory_with_one_archive_and_a_shot_list) {
    testing::TempDir dir;
    write_archive(dir / "a.zip", "fcv1", "msvc",
                  {{"s0", 0, quiet_raw(), ""}, {"s1", 1, twice_raw(), ""}, {"s2", 2, quiet_raw(), ""}});
    write_file_new(dir / "ids.txt", "s1\r\n\r\n  s1  \n");
    CHECK_EQ(run_render({p(dir / "a.zip"), "--shots", p(dir / "ids.txt"), "--output", p(dir / "out")}), 0);
    std::map<std::string, std::string> files = contents_of(dir / "out");
    CHECK_EQ(files.size(), static_cast<std::size_t>(2));
    CHECK(files.count("1.svg") == 1 && files.count("render.json") == 1);
    Json report = read_json(dir / "out" / "render.json");
    CHECK(report["inputs"]["b"].is_null());
    CHECK_EQ(report["shots"].size(), static_cast<std::size_t>(1));
    CHECK(report["shots"][0]["sources"].contains("a"));
    CHECK(!report["shots"][0]["sources"].contains("b"));
    CHECK_EQ(report["shots"][0]["sources"]["a"]["collision_count"].get<int>(), 2);
    CHECK(report["a_only"].empty() && report["b_only"].empty());
    CHECK(report["contract_differences"].empty());

    // an empty selection is valid: no SVG, `shots` is empty
    write_file_new(dir / "none.txt", "\n\n");
    CHECK_EQ(run_render({p(dir / "a.zip"), "--shots", p(dir / "none.txt"), "--output", p(dir / "empty")}), 0);
    CHECK_EQ(contents_of(dir / "empty").size(), static_cast<std::size_t>(1));
    CHECK(read_json(dir / "empty" / "render.json")["shots"].empty());
}

TEST(render_trajectory_refuses_what_it_must) {
    testing::TempDir dir;
    write_archive(dir / "a.zip", "fcv1", "msvc", {{"s0", 0, quiet_raw(), ""}, {"s1", 1, quiet_raw(), ""}});
    write_archive(dir / "dup.zip", "fcv1", "msvc", {{"s0", 5, quiet_raw(), ""}, {"s1", 5, quiet_raw(), ""}});
    write_archive(dir / "moved.zip", "fcv1", "msvc", {{"s0", 7, quiet_raw(), ""}, {"s1", 1, quiet_raw(), ""}});
    write_archive(dir / "bad.zip", "fcv1", "msvc", {{"s0", 0, quiet_raw(), ""}, {"s1", 1, "", "not a trajectory"}});

    // an existing directory (empty or not) and an existing file are refused and left untouched
    std::filesystem::create_directory(dir / "exists");
    CHECK(throws({p(dir / "a.zip"), "--output", p(dir / "exists")}));
    CHECK(std::filesystem::is_directory(dir / "exists") && std::filesystem::is_empty(dir / "exists"));
    write_file_new(dir / "file", "keep");
    CHECK(throws({p(dir / "a.zip"), "--output", p(dir / "file")}));
    CHECK_EQ(read_file(dir / "file"), std::string("keep"));
    CHECK(throws({p(dir / "a.zip"), "--output", p(dir / "missing-parent" / "out")}));
    CHECK(!std::filesystem::exists(dir / "missing-parent"));

    // an unknown shot id in --shots names it and creates nothing
    write_file_new(dir / "ids.txt", "s0\nnope\n");
    std::string message;
    try {
        run_render({p(dir / "a.zip"), "--shots", p(dir / "ids.txt"), "--output", p(dir / "o1")});
    } catch (const DcDeterminismError& error) {
        message = error.what();
    }
    CHECK(message.find("nope") != std::string::npos);
    CHECK(!std::filesystem::exists(dir / "o1"));

    // an id present in only one of two archives is accepted (listed, not rendered)
    write_archive(dir / "b.zip", "fcv1plus", "msvc", {{"s0", 0, quiet_raw(), ""}, {"only-b", 9, quiet_raw(), ""}});
    write_file_new(dir / "ids2.txt", "s1\nonly-b\n");
    CHECK_EQ(run_render({p(dir / "a.zip"), p(dir / "b.zip"), "--shots", p(dir / "ids2.txt"), "--output", p(dir / "o2")}), 0);
    Json listed = read_json(dir / "o2" / "render.json");
    CHECK(listed["shots"].empty());
    CHECK_EQ(listed["a_only"][0].get<std::string>(), std::string("s1"));
    CHECK_EQ(listed["b_only"][0].get<std::string>(), std::string("only-b"));

    // equal corpus indexes among the selected shots, and different indexes of one shot in A and B
    CHECK(throws({p(dir / "dup.zip"), "--output", p(dir / "o3")}));
    CHECK(!std::filesystem::exists(dir / "o3"));
    CHECK(throws({p(dir / "a.zip"), p(dir / "moved.zip"), "--output", p(dir / "o4")}));
    CHECK(!std::filesystem::exists(dir / "o4"));

    // a shot that cannot be decoded removes everything the command wrote
    CHECK(throws({p(dir / "bad.zip"), "--output", p(dir / "o5"), "--workers", "1"}));
    CHECK(!std::filesystem::exists(dir / "o5"));
    CHECK(throws({p(dir / "does-not-exist.zip"), "--output", p(dir / "o6")}));
    CHECK(!std::filesystem::exists(dir / "o6"));
}

TEST(render_trajectory_keeps_the_shot_id_in_render_json_and_the_svg_valid) {
    testing::TempDir dir;
    std::string id = std::string("id&<\"") + '\0' + '\x01' + "/end:0/shot:0";
    write_archive(dir / "a.zip", "fcv1", "msvc", {{id, 0, twice_raw(), ""}});
    CHECK_EQ(run_render({p(dir / "a.zip"), "--output", p(dir / "out")}), 0);
    std::string svg = read_file(dir / "out" / "0.svg");
    std::string problem;
    CHECK(well_formed(svg, problem));
    if (!problem.empty()) ::testing::report_failure(__FILE__, __LINE__, "not well-formed: " + problem);
    Json report = read_json(dir / "out" / "render.json");
    CHECK(report["shots"][0]["shot_id"].get<std::string>() == id);
}

// ---------------------------------------------------------------------------------- header and log overlay

TEST(the_header_is_centred) {
    std::vector<RenderedSource> sources = {rendered("A", "#00a86b", source_of(head_on_frames())),
                                           rendered("B", "#2563eb", source_of(head_on_frames()))};
    std::string svg = render_shot_svg("shot-x", sources);
    CHECK(svg.find("<text x=\"490.0\" y=\"22\" class=\"overlay-title\" text-anchor=\"middle\">shot shot-x</text>") !=
          std::string::npos);
    // both legend swatches start at the same x, which is left of the centre
    std::size_t first = svg.find("<line x1=\"");
    std::size_t second = svg.find("<line x1=\"", first + 1);
    CHECK(first != std::string::npos && second != std::string::npos);
    std::string x_first = svg.substr(first + 10, svg.find('"', first + 10) - (first + 10));
    std::string x_second = svg.substr(second + 10, svg.find('"', second + 10) - (second + 10));
    CHECK_EQ(x_first, x_second);
    CHECK(std::stod(x_first) > 14.0 && std::stod(x_first) < 490.0);
}

namespace {

Board board_with(std::initializer_list<std::pair<int, std::pair<double, double>>> stones) {
    Board board;
    for (const auto& item : stones) {
        board[static_cast<std::size_t>(item.first)].present = true;
        board[static_cast<std::size_t>(item.first)].x = item.second.first;
        board[static_cast<std::size_t>(item.first)].y = item.second.second;
    }
    return board;
}

RenderedSource log_rendered(const std::vector<Board>& boards) {
    RenderedSource source;
    source.log = true;
    source.tag = "log";
    source.label = "log  frames=" + std::to_string(boards.size()) + "  seconds_per_frame=0.1";
    source.color = "#8f8f8f";
    source.status = "recorded";
    source.data = make_log_render_source(boards);
    return source;
}

}  // namespace

TEST(the_log_overlay_is_split_at_absences_and_has_no_final_circle_for_a_removed_stone) {
    // stone 0 moves, disappears in board 3 and comes back; stone 1 is removed at the end; stone 2 never moves
    std::vector<Board> boards = {
        board_with({{0, {0.0, 34.0}}, {1, {0.5, 38.0}}, {2, {-1.0, 36.0}}}),
        board_with({{0, {0.0, 34.5}}, {1, {0.5, 38.0}}, {2, {-1.0, 36.0}}}),
        board_with({{0, {0.0, 35.0}}, {1, {0.5, 38.5}}, {2, {-1.0, 36.0}}}),
        board_with({{1, {0.5, 39.0}}, {2, {-1.0, 36.0}}}),
        board_with({{0, {0.0, 36.0}}, {1, {0.5, 39.5}}, {2, {-1.0, 36.0}}}),
        board_with({{0, {0.0, 36.5}}, {2, {-1.0, 36.0}}}),
    };
    RenderSource data = make_log_render_source(boards);
    CHECK_EQ(data.frames, static_cast<std::int64_t>(6));
    CHECK_EQ(data.stones.size(), static_cast<std::size_t>(3));
    std::vector<RenderedSource> sources = {rendered("A", "#00a86b", source_of(head_on_frames())), log_rendered(boards)};
    std::string svg = render_shot_svg("log-shot", sources);
    std::string problem;
    CHECK(well_formed(svg, problem));
    // stone 0: two pieces, stone 1: one piece; stone 2 did not move (plus the legend swatch)
    CHECK_EQ(count_of(svg, "stroke=\"#8f8f8f\" stroke-width=\"1.15\" stroke-opacity=\"1.00\"/>"), static_cast<std::size_t>(4));
    // final grey circles: stones 0 and 2 (stone 1 was removed)
    CHECK_EQ(count_of(svg, "fill=\"#8f8f8f\" fill-opacity=\"0.08\""), static_cast<std::size_t>(2));
    CHECK(svg.find("log  frames=6  seconds_per_frame=0.1") != std::string::npos);
    CHECK(svg.find("log: recorded positions, no collisions inferred.") != std::string::npos);
}

namespace {

std::string logged_stone(double x, double y) {
    return "{\"position\":{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}}";
}

// A plain-file game `g` with one shot (end 0, shot 0): stone team0[0] slides from y 33 to y 38.
void write_logs(const std::filesystem::path& root, bool malformed = false) {
    std::filesystem::create_directories(root / "g");
    std::string start = "{\"team0\":[null],\"team1\":[]}";
    std::string frames = "[";
    for (int t = 0; t <= 10; ++t) {
        if (t != 0) frames += ",";
        frames += "[{\"team\":\"team0\",\"index\":0,\"value\":" + logged_stone(0.0, 33.0 + 0.5 * t) + "}]";
    }
    frames += "]";
    std::string trajectory = malformed ? "{\"start\":" + start + ",\"frames\":7,\"seconds_per_frame\":0.1}"
                                       : "{\"start\":" + start + ",\"frames\":" + frames +
                                             ",\"seconds_per_frame\":0.1,\"finish\":" + start + "}";
    std::string text = "{\"log\":{\"game_id\":\"g\",\"end\":0,\"shot\":0,\"actual_move\":{\"type\":\"shot\","
                       "\"velocity\":{\"x\":0.25,\"y\":2.5},\"rotation\":\"ccw\"},\"trajectory\":" +
                       trajectory + "}}";
    write_file_new(root / "g" / "shot_e000s00.json", text);
}

}  // namespace

TEST(render_trajectory_draws_the_logs) {
    testing::TempDir dir;
    write_logs(dir / "logs");
    write_archive(dir / "a.zip", "fcv1", "msvc",
                  {{"g/end:0/shot:0", 0, twice_raw(), ""}, {"g/end:0/shot:1", 1, quiet_raw(), ""}});
    CHECK_EQ(run_render({p(dir / "a.zip"), "--logs-dir", p(dir / "logs"), "--output", p(dir / "out")}), 0);
    Json report = read_json(dir / "out" / "render.json");
    CHECK(report["logs"].is_object());
    CHECK(!report["logs"]["matches_contract_a"].get<bool>());
    CHECK_EQ(report["log_missing"].size(), static_cast<std::size_t>(1));
    CHECK_EQ(report["log_missing"][0].get<std::string>(), std::string("g/end:0/shot:1"));
    CHECK_EQ(report["shots"][0]["log"]["frames"].get<std::int64_t>(), static_cast<std::int64_t>(12));
    CHECK(report["shots"][0]["log"]["seconds_per_frame"].get<double>() == 0.1);
    CHECK(report["shots"][1]["log"].is_null());
    CHECK(!report["shots"][0]["sources"].contains("log"));
    std::string svg = read_file(dir / "out" / "0.svg");
    CHECK(svg.find("stroke=\"#8f8f8f\" stroke-width=\"1.15\"") != std::string::npos);
    CHECK(read_file(dir / "out" / "1.svg").find("#8f8f8f") == std::string::npos);

    // the same with 4 workers gives the same SVG bytes
    CHECK_EQ(run_render({p(dir / "a.zip"), "--logs-dir", p(dir / "logs"), "--output", p(dir / "out4"), "--workers", "4"}),
             0);
    CHECK_EQ(read_file(dir / "out4" / "0.svg"), svg);

    // without --logs-dir: logs null, log_missing empty
    CHECK_EQ(run_render({p(dir / "a.zip"), "--output", p(dir / "plain")}), 0);
    Json plain = read_json(dir / "plain" / "render.json");
    CHECK(plain["logs"].is_null());
    CHECK(plain["log_missing"].empty());
    CHECK(plain["shots"][0]["log"].is_null());

    // an empty logs directory, an empty value and a malformed log are errors; nothing is left behind
    std::filesystem::create_directories(dir / "empty-logs");
    CHECK(throws({p(dir / "a.zip"), "--logs-dir", p(dir / "empty-logs"), "--output", p(dir / "o1")}));
    CHECK(!std::filesystem::exists(dir / "o1"));
    CHECK(throws({p(dir / "a.zip"), "--logs-dir", "", "--output", p(dir / "o2")}));
    CHECK(!std::filesystem::exists(dir / "o2"));
    write_logs(dir / "bad-logs", true);
    CHECK(throws({p(dir / "a.zip"), "--logs-dir", p(dir / "bad-logs"), "--output", p(dir / "o3"), "--workers", "1"}));
    CHECK(!std::filesystem::exists(dir / "o3"));
}

int main() { return testing::run_all_tests(); }
