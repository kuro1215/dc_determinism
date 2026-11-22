// Unit tests of the capture side: dctr-v1, capture archives, statistics and the trajectory
// comparison.
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

#include "capture/archive.hpp"
#include "capture/compare_traj.hpp"
#include "capture/dctr.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/metrics.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

std::uint32_t bits_of(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void put32(std::string& out, std::uint32_t value) {
    for (int b = 0; b < 4; ++b) out.push_back(static_cast<char>((value >> (8 * b)) & 0xFF));
}

// frames[f][s] = six float32 values of the s-th active stone.
std::string raw_of(std::uint32_t mask, const std::vector<std::vector<std::array<std::uint32_t, 6>>>& frames) {
    std::string raw;
    put32(raw, mask);
    put32(raw, static_cast<std::uint32_t>(frames.size()));
    for (const auto& frame : frames) {
        for (const auto& state : frame) {
            for (std::uint32_t value : state) put32(raw, value);
        }
    }
    return raw;
}

std::array<std::uint32_t, 6> state(float x, float y, float angle = 0.0f, float vx = 0.0f, float vy = 0.0f,
                                   float w = 0.0f) {
    return {bits_of(x), bits_of(y), bits_of(angle), bits_of(vx), bits_of(vy), bits_of(w)};
}

}  // namespace

TEST(raw_round_trip_keeps_every_bit) {
    std::vector<std::vector<std::array<std::uint32_t, 6>>> frames = {
        {state(0.5f, 38.0f), state(-0.0f, 1.5f, 3.0f, -0.0f)},
        {state(0.5f, 37.0f, 1.0f, 2.0f, 3.0f, 4.0f), {0x7FC00001u, 0xFF800000u, 0, 1, 0x80000000u, 0xFFFFFFFFu}},
        {state(0.5f, 36.0f), state(0.0f, 1.5f)}};
    std::string raw = raw_of(0x00000005u, frames);  // stones 0 and 2
    std::int64_t count = 0;
    std::string payload = raw_to_payload(raw, "shot-1", 7, "fcv1", &count);
    CHECK_EQ(count, 3);
    CapturedTrajectory trajectory(payload);
    CHECK_EQ(trajectory.frames(), 3);
    CHECK_EQ(trajectory.stones().size(), static_cast<std::size_t>(2));
    CHECK_EQ(trajectory.stones()[1], 2);
    CHECK(trajectory.has_stone(2));
    CHECK(!trajectory.has_stone(1));
    CHECK_EQ(trajectory.to_raw(), raw);
    std::vector<std::uint32_t> y_column = trajectory.column_bits(0, 1);  // (a reference into a temporary would dangle)
    std::vector<std::uint32_t> w_column = trajectory.column_bits(2, 5);
    CHECK_EQ(y_column[2], bits_of(36.0f));
    CHECK_EQ(w_column[1], 0xFFFFFFFFu);
    CHECK_EQ(trajectory.header()["shot_id"].get<std::string>(), std::string("shot-1"));
    CHECK_EQ(trajectory.header()["engine"].get<std::string>(), std::string("fcv1"));
    CHECK_EQ(payload.compare(0, 8, "DCTR0001"), 0);
}

TEST(empty_trajectories) {
    CapturedTrajectory no_frames(raw_to_payload(raw_of(0x3u, {}), "s", 0, "fcv1"));
    CHECK_EQ(no_frames.frames(), 0);
    CHECK_EQ(no_frames.column_bits(1, 0).size(), static_cast<std::size_t>(0));
    CapturedTrajectory no_stones(raw_to_payload(raw_of(0u, {{}, {}}), "s", 0, "fcv1"));
    CHECK_EQ(no_stones.frames(), 2);
    CHECK(no_stones.stones().empty());
}

TEST(raw_files_are_validated) {
    CHECK_THROWS(parse_raw("abc"));
    std::string raw = raw_of(0x1u, {{state(1, 2)}});
    CHECK_EQ(parse_raw(raw).frames, 1u);
    CHECK_THROWS(parse_raw(raw.substr(0, raw.size() - 1)));
    CHECK_THROWS(parse_raw(raw + "x"));
}

TEST(payloads_are_validated) {
    std::string payload = raw_to_payload(raw_of(0x1u, {{state(1, 2)}, {state(1, 3)}}), "s", 0, "fcv1");
    CHECK_THROWS(CapturedTrajectory{payload.substr(0, 5)});
    std::string wrong_magic = payload;
    wrong_magic[0] = 'X';
    CHECK_THROWS(CapturedTrajectory{wrong_magic});
    CHECK_THROWS(CapturedTrajectory{payload.substr(0, 14)});              // truncated header
    CHECK_THROWS(CapturedTrajectory{payload.substr(0, payload.size() - 1)});  // short body
    CHECK_THROWS(CapturedTrajectory{payload + "x"});
}

TEST(distribution_follows_python) {
    Json empty = distribution({});
    CHECK_EQ(empty["count"].get<int>(), 0);
    CHECK(empty["max"].is_null());
    CHECK(empty["p50"].is_null());
    Json one = distribution({2.5});
    CHECK_EQ(one["p99"].get<double>(), 2.5);
    Json four = distribution({4.0, 1.0, 3.0, 2.0});
    CHECK_EQ(four["max"].get<double>(), 4.0);
    CHECK_EQ(four["p50"].get<double>(), 2.5);
    CHECK(four["max"].is_number_float());
    Json ints = distribution_ints({3, 9, 5});
    CHECK(ints["max"].is_number_integer());  // Python max() of ints is an int ...
    CHECK(ints["p50"].is_number_float());    // ... the percentiles are floats
    CHECK_EQ(ints["p50"].get<double>(), 5.0);
    Json above = threshold_counts({0.0, 1e-7, 1e-6, 2e-6, 0.5});
    CHECK_EQ(above["gt_0"].get<int>(), 4);
    CHECK_EQ(above["gt_1e-6"].get<int>(), 2);  // strictly greater
    CHECK_EQ(above["gt_1e-2"].get<int>(), 1);
}

TEST(archive_round_trip) {
    testing::TempDir dir;
    std::filesystem::path archive_path = dir / "capture.zip";
    std::string payload = raw_to_payload(raw_of(0x1u, {{state(1, 2)}, {state(1, 3)}}), "shot-a", 4, "fcv1");
    {
        CaptureWriter writer(archive_path);
        writer.add(capture_member_name(4), payload);
        Json row = Json::object();
        row["index"] = 4;
        row["shot_id"] = "shot-a";
        row["status"] = "stopped";
        row["steps"] = 12;
        row["trajectory_digest"] = std::string(64, 'a');
        row["final_digest"] = std::string(64, 'b');
        row["frames"] = 2;
        row["member"] = capture_member_name(4);
        Json contract = Json::object();
        for (const char* key : {"engine", "prepared_sha256", "record_digest", "corpus_input_sha256"}) contract[key] = "x";
        contract["observation"] = "raw-stonecoordinate-v1";
        contract["max_steps"] = 60000;
        Json index = Json::object();
        index["schema"] = kCaptureSchema;
        index["format"] = kDctrFormat;
        index["contract"] = contract;
        index["shots"] = Json::array({row});
        writer.commit(index);
    }
    CHECK_THROWS(CaptureWriter{archive_path});  // never overwrites
    CaptureArchive archive(path_to_utf8(archive_path));
    CHECK_EQ(archive.shot_ids().size(), static_cast<std::size_t>(1));
    CHECK(archive.has("shot-a"));
    CHECK(!archive.has("shot-b"));
    auto reader = archive.open_reader();
    CapturedTrajectory trajectory = archive.trajectory("shot-a", *reader);
    CHECK_EQ(trajectory.frames(), 2);
    std::vector<std::uint32_t> y_column = trajectory.column_bits(0, 1);
    CHECK_EQ(y_column[1], bits_of(3.0f));
    write_file_new(dir / "plain.zip", "not a zip");
    CHECK_THROWS(CaptureArchive{path_to_utf8(dir / "plain.zip")});
}

namespace {

// A one-shot archive of `engine` carrying `provenance`.
void write_one_shot_archive(const std::filesystem::path& path, const std::string& engine, const Json& provenance,
                            const std::string& schema = kCaptureSchema) {
    CaptureWriter writer(path);
    writer.add(capture_member_name(0), raw_to_payload(raw_of(0x1u, {{state(1, 2)}, {state(1, 3)}}), "shot-a", 0, engine));
    Json row = Json::object();
    row["index"] = 0;
    row["shot_id"] = "shot-a";
    row["status"] = "stopped";
    row["steps"] = 12;
    row["trajectory_digest"] = nullptr;
    row["final_digest"] = nullptr;
    row["frames"] = 2;
    row["member"] = capture_member_name(0);
    Json contract = Json::object();
    contract["engine"] = engine;
    contract["observation"] = "raw-stonecoordinate-v1";
    contract["max_steps"] = 60000;
    contract["prepared_sha256"] = std::string(64, 'p');
    contract["record_digest"] = std::string(64, 'r');
    contract["corpus_input_sha256"] = std::string(64, 'c');
    Json index = Json::object();
    index["schema"] = schema;
    index["format"] = kDctrFormat;
    index["contract"] = contract;
    index["provenance"] = provenance;
    index["shots"] = Json::array({row});
    writer.commit(index);
}

}  // namespace

TEST(compare_archives_reports_no_paths_but_keeps_the_other_members) {
    testing::TempDir dir;
    Json loader = Json::object({{"path", "/loader/dc_determinism.so"}, {"sha256", std::string(64, '1')}, {"origin", "prebuilt"}});
    Json manifest_entry = Json::object({{"path", "prebuilt/pybind/py27/engine.so"}, {"sha256", std::string(64, '2')}});
    Json binary = Json::object({{"path", "/binary/engine.so"},
                                {"sha256", std::string(64, '2')},
                                {"manifest_entry", manifest_entry},
                                {"sidecars", Json::array({Json::object({{"path", "x"}, {"kind", "k"}})})},
                                {"kind", "record-v2"}});
    write_one_shot_archive(dir / "a.zip", "fcv1", Json::object({{"loader", loader}, {"environment", Json::object()}}));
    write_one_shot_archive(dir / "b.zip", "pybind-py27",
                           Json::object({{"engine_binary", binary}, {"environment", Json::object()}}));
    CHECK_EQ(compare_archives(path_to_utf8(dir / "a.zip"), path_to_utf8(dir / "b.zip"), dir / "out.json", 1), 0);
    Json report = read_json(dir / "out.json");
    CHECK_EQ(report["schema"], Json("dc-determinism-capture-comparison-v2"));
    CHECK(!testing::has_path_key(report["inputs"]));
    CHECK_EQ(report["inputs"][0]["sha256"], Json(sha256_file(dir / "a.zip")));
    CHECK_EQ(report["inputs"][0]["loader"]["sha256"], Json(std::string(64, '1')));
    CHECK(report["inputs"][0]["engine_binary"].is_null());
    const Json& copied = report["inputs"][1]["engine_binary"];
    CHECK_EQ(copied["sha256"], Json(std::string(64, '2')));
    CHECK_EQ(copied["manifest_entry"]["sha256"], Json(std::string(64, '2')));
    CHECK_EQ(copied["sidecars"][0]["kind"], Json("k"));
    std::vector<std::string> keys;
    for (auto it = copied.begin(); it != copied.end(); ++it) keys.push_back(it.key());
    CHECK((keys == std::vector<std::string>{"sha256", "manifest_entry", "sidecars", "kind"}));
}

TEST(archives_with_another_schema_name_are_refused) {
    testing::TempDir dir;
    Json loader = Json::object({{"path", "/loader/dc_determinism.so"}, {"sha256", std::string(64, '1')}, {"origin", "prebuilt"}});
    Json provenance = Json::object({{"loader", loader}, {"environment", Json::object()}});
    write_one_shot_archive(dir / "current.zip", "fcv1", provenance, kCaptureSchema);
    CaptureArchive archive(path_to_utf8(dir / "current.zip"));
    CHECK(archive.has("shot-a"));
    write_one_shot_archive(dir / "other.zip", "fcv1", provenance, "dc-determinism-capture-v2");
    CHECK_THROWS(CaptureArchive{path_to_utf8(dir / "other.zip")});
}

TEST(trajectory_comparison) {
    std::vector<std::vector<std::array<std::uint32_t, 6>>> base = {
        {state(0.0f, 38.0f), state(0.1f, 37.0f)}, {state(0.0f, 38.5f), state(0.1f, 37.5f)}, {state(0.0f, 39.0f), state(0.1f, 38.0f)}};
    auto changed = base;
    changed[1][1][1] = bits_of(37.5f + 1e-3f);  // stone 1, y, from frame 1 on
    changed[2][1][1] = bits_of(38.0f + 1e-3f);
    CapturedTrajectory a(raw_to_payload(raw_of(0x3u, base), "s", 0, "fcv1"));
    CapturedTrajectory same(raw_to_payload(raw_of(0x3u, base), "s", 0, "fcv1plus"));
    CapturedTrajectory b(raw_to_payload(raw_of(0x3u, changed), "s", 0, "fcv1plus"));

    Json identical = compare_trajectories(a, same);
    CHECK(!identical["differing"].get<bool>());
    CHECK(identical["first_diff"].is_null());
    CHECK_EQ(identical["final"]["max_position_distance"].get<double>(), 0.0);

    Json result = compare_trajectories(a, b);
    CHECK(result["differing"].get<bool>());
    CHECK_EQ(result["differing_frames"].get<int>(), 2);
    CHECK_EQ(result["first_diff"]["step"].get<int>(), 1);
    CHECK_EQ(result["first_diff"]["field"].get<std::string>(), std::string("y"));
    CHECK_EQ(result["first_diff"]["stone"].get<int>(), 1);
    CHECK_EQ(result["max_diff"]["position"]["stone"].get<int>(), 1);
    CHECK(result["max_diff"]["position"]["value"].get<double>() > 9e-4);
    CHECK(result["max_diff"]["velocity"]["step"].is_null());
    CHECK(result.contains("outcome"));
    CHECK_EQ(result["presence"]["mismatch_pairs"].get<int>(), 0);

    // A shorter trajectory is continued with its final state; an absent stone counts as a presence mismatch.
    CapturedTrajectory shorter(raw_to_payload(raw_of(0x1u, {{base[0][0]}}), "s", 0, "fcv1"));
    Json longer = compare_trajectories(a, shorter);
    CHECK_EQ(longer["frames_a"].get<int>(), 3);
    CHECK_EQ(longer["frames_b"].get<int>(), 1);
    CHECK_EQ(longer["common_frames"].get<int>(), 1);
    CHECK_EQ(longer["final"]["presence_mismatch_stones"].size(), static_cast<std::size_t>(1));
    CHECK(longer["presence"]["mismatch_pairs"].get<int>() > 0);
}

TEST(summary_counts_truncated_separately) {
    Json rows = Json::array();
    for (int i = 0; i < 3; ++i) {
        Json row = Json::object();
        row["shot_id"] = "s" + std::to_string(i);
        row["truncated"] = i == 2;
        row["differing"] = i == 1;
        row["step_count_difference"] = i;
        Json diff = Json::object();
        diff["step"] = 5;
        diff["field"] = "vx";
        diff["fields"] = Json::array({"vx"});
        row["first_diff"] = i == 1 ? diff : Json(nullptr);
        row["final"] = Json::object({{"max_position_distance", 0.25}, {"presence_mismatch_stones", Json::array()}});
        Json maximum = Json::object({{"value", 0.5}});
        row["max_diff"] = Json::object({{"position", maximum}, {"velocity", maximum}, {"angle", maximum}, {"w", maximum}});
        row["presence"] = Json::object({{"mismatch_pairs", 0}});
        rows.push_back(row);
    }
    Json summary = summarize_trajectory_rows(rows);
    CHECK_EQ(summary["shots"].get<int>(), 3);
    CHECK_EQ(summary["truncated"].get<int>(), 1);
    CHECK_EQ(summary["analysed"].get<int>(), 2);
    CHECK_EQ(summary["differing"].get<int>(), 1);
    CHECK_EQ(summary["identical"].get<int>(), 1);
    CHECK_EQ(summary["velocity_only_first_diff"].get<int>(), 1);
    CHECK_EQ(summary["first_diff_field_counts"]["vx"].get<int>(), 1);
    CHECK_EQ(summary["step_count_difference"].get<int>(), 1);
    CHECK_EQ(summary["final_displacement_m"]["count"].get<int>(), 2);
}

int main() { return testing::run_all_tests(); }
