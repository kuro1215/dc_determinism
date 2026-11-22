// Unit tests of the corpus layer: Python-compatible JSON parsing and text helpers, game log
// classification, sheet width, the initial coordinate, the catalogue (plain files and ZIP files),
// prepared.bin encoding and the catalogue rebuild behind a prepared directory.
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "corpus/capi_initial.hpp"
#include "corpus/catalogue.hpp"
#include "corpus/prepared.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"
#include "testing.hpp"
#include "tempdir.hpp"
#include "zip/zip.hpp"

using namespace dc_determinism;

namespace {

bool parse(const std::string& text, Json& out, const JsonKeepTree* keep = nullptr) {
    std::string error;
    return py_json_loads(text, out, error, keep);
}

float stone_float(const Coordinate& coord, std::size_t sid, std::size_t field) {
    std::uint32_t bits = 0;
    const std::uint8_t* p = coord.data() + (sid * 6 + field) * 4;
    bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string shot_text(const std::string& game_id, int end, int shot, const std::string& extra = std::string(),
                      const std::string& move = "{\"type\":\"shot\",\"velocity\":{\"x\":0.25,\"y\":2.5},"
                                                "\"rotation\":\"ccw\"}") {
    std::string text = "{\"log\":{";
    if (!game_id.empty()) text += "\"game_id\":\"" + game_id + "\",";
    text += "\"end\":" + std::to_string(end) + ",\"shot\":" + std::to_string(shot) + ",\"actual_move\":" + move;
    text += extra + "}}";
    return text;
}

const char* kConfig = "{\"game\":{\"setting\":{\"sheet_width\":4.5}}}";

}  // namespace

TEST(parse_python_extensions) {
    Json value;
    CHECK(parse("[NaN, Infinity, -Infinity, -0.0, 1e2, 12345678901234567890123, -0]", value));
    CHECK(std::isnan(value[0].get<double>()));
    CHECK(std::isinf(value[1].get<double>()) && value[1].get<double>() > 0);
    CHECK(std::isinf(value[2].get<double>()) && value[2].get<double>() < 0);
    CHECK(std::signbit(value[3].get<double>()));
    CHECK(value[4].is_number_float());
    CHECK(value[5].is_number_float());
    CHECK(value[6].is_number_integer() && value[6].get<std::int64_t>() == 0);
    CHECK(parse("{\"a\":1,\"b\":2,\"a\":3}", value));
    CHECK_EQ(json_dumps(value, DumpOptions::compact()), std::string("{\"a\":3,\"b\":2}"));
    CHECK(parse(" \t\r\n{\"s\":\"\\u00e9\\ud83d\\ude00\\n\"} ", value));
    CHECK_EQ(value["s"].get<std::string>(), std::string("\xc3\xa9\xf0\x9f\x98\x80\n"));
    CHECK(parse("18446744073709551615", value) && value.is_number_unsigned());
}

TEST(parse_python_errors) {
    Json value;
    for (const char* bad : {"", "01", "[1,]", "{\"a\":1,}", "[1 2]", "\"a\nb\"", "\"\\x\"", "\"\\u12\"", "{1:2}",
                            "nul", "[1] x", "1.", "-", "\xef\xbb\xbf{}", "{\"a\" 1}", "[", "\"abc"}) {
        CHECK(!parse(bad, value));
    }
    CHECK(parse("1.5e", value) == false);  // "1.5" is consumed, "e" is extra data
    CHECK(parse("[1.5e]", value) == false);
}

TEST(parse_keep_tree) {
    JsonKeepTree keep;
    keep.add({"log", "end"});
    keep.add({"log", "trajectory", "frames"});
    Json value;
    CHECK(parse("{\"log\":{\"end\":3,\"x\":[1,2],\"trajectory\":{\"frames\":[[1]],\"finish\":{}}},\"y\":1}", value, &keep));
    CHECK_EQ(json_dumps(value, DumpOptions::compact()), std::string("{\"log\":{\"end\":3,\"trajectory\":{\"frames\":[[1]]}}}"));
    CHECK(!parse("{\"log\":{\"x\":[1,}}", value, &keep));  // skipped parts are still validated
}

TEST(python_text_helpers) {
    CHECK(is_valid_utf8("abc \xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(!is_valid_utf8("\xc0\x80"));
    CHECK(!is_valid_utf8("\xed\xa0\x80"));
    CHECK(!is_valid_utf8("\xf4\x90\x80\x80"));
    CHECK(!is_valid_utf8("\xe3\x81"));
    CHECK(py_is_blank(""));
    CHECK(py_is_blank(" \t\n\xc2\xa0\xe3\x80\x80\x1f"));
    CHECK(!py_is_blank(" a "));
    std::vector<std::string> lines = py_splitlines("a\nb\r\nc\rd\x0b" "e\x0c" "f\x1c" "g\xc2\x85h\xe2\x80\xa8i\n");
    CHECK_EQ(lines.size(), static_cast<std::size_t>(9));
    CHECK_EQ(lines[8], std::string("i"));
    CHECK_EQ(py_splitlines("a\n\nb").size(), static_cast<std::size_t>(3));
    CHECK_EQ(py_splitlines("").size(), static_cast<std::size_t>(0));
    CHECK(is_shot_name("shot_e000s00.json"));
    CHECK(is_shot_name("shot_e.json"));  // fnmatch: `*` also matches nothing
    CHECK(!is_shot_name("shot_.json"));
    CHECK(!is_shot_name("shot_e1.JSON"));
    CHECK(is_zip_name("A.ZIP") && !is_zip_name("zip"));
    std::int64_t number = 0;
    std::string reason;
    CHECK(py_int(Json(3.9), number, reason) && number == 3);
    CHECK(py_int(Json(-3.9), number, reason) && number == -3);
    CHECK(py_int(Json(true), number, reason) && number == 1);
    CHECK(py_int(Json(" -12 "), number, reason) && number == -12);
    CHECK(!py_int(Json("1.5"), number, reason));
    CHECK(!py_int(Json(), number, reason));
    CHECK(!py_int(Json(1e30), number, reason));
    double real = 0;
    CHECK(py_float(Json("4.5"), real, reason) && real == 4.5);
    CHECK(py_float(Json(7), real, reason) && real == 7.0);
    CHECK(!py_float(Json("x"), real, reason));
    CHECK(!py_float(Json(), real, reason));
}

TEST(fgz_classification_rules) {
    std::string text =
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":0,\"shot\":1},\"last_move\":{\"free_guard_zone_foul\":true}}}\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":0,\"shot\":1},\"last_move\":{\"free_guard_zone_foul\":false}}}\n"
        "{\"log\":{\"cmd\":\"other\",\"state\":{\"end\":0,\"shot\":9},\"last_move\":{\"free_guard_zone_foul\":true}}}\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":1.0,\"shot\":3},\"last_move\":{\"free_guard_zone_foul\":false}}}\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":2,\"shot\":3},\"last_move\":{\"free_guard_zone_foul\":null}}}\n"
        "[1]\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":NaN,\"shot\":3},\"last_move\":{\"free_guard_zone_foul\":true}}}\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":4,\"shot\":5},\"last_move\":{\"free_guard_zone_foul\":true}}}\n"
        "not json\n"
        "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":6,\"shot\":7},\"last_move\":{\"free_guard_zone_foul\":true}}}\n";
    FgzMap found = fgz_classifications(text);
    CHECK_EQ(found.size(), static_cast<std::size_t>(3));
    auto key = [](int end, int shot) { return std::make_pair(std::int64_t(end), std::int64_t(shot)); };
    CHECK_EQ(found[key(0, 0)], std::string("rule_restore"));  // the first update wins
    CHECK_EQ(found[key(1, 2)], std::string("none_detected"));
    CHECK_EQ(found[key(4, 4)], std::string("rule_restore"));
    CHECK(found.count(key(6, 6)) == 0);  // after the malformed line
    CHECK(fgz_classifications("\n{}").empty());  // an empty line is malformed JSON
}

TEST(sheet_width_rules) {
    SheetWidth width = sheet_width_from_config(kConfig);
    CHECK(width.width == 4.5 && width.source == "config");
    CHECK(sheet_width_from_config("{\"game\":{\"setting\":{\"sheet_width\":\"5\"}}}").width == 5.0);
    CHECK_THROWS(sheet_width_from_config("{\"game\":{}}"));
    CHECK_THROWS(sheet_width_from_config("not json"));
    CHECK_THROWS(sheet_width_from_config("{\"game\":{\"setting\":{\"sheet_width\":null}}}"));
}

TEST(initial_coordinate_first_shot) {
    Json log;
    CHECK(parse(shot_text("g", 0, 0), log));
    CapiInitial initial = build_capi_initial(log["log"]);
    CHECK_EQ(initial.delivered_sid, 0);
    CHECK(stone_float(initial.coord, 0, 3) == 0.25f && stone_float(initial.coord, 0, 4) == 2.5f);
    CHECK(stone_float(initial.coord, 0, 5) == static_cast<float>(3.141592653589793 / 2.0));
    for (std::size_t i = 1; i < kStoneCount; ++i) {
        for (std::size_t f = 0; f < 6; ++f) CHECK(stone_float(initial.coord, i, f) == 0.0f);
    }
}

TEST(initial_coordinate_storage_and_side) {
    std::string storage =
        ",\"simulator_storage\":{\"stones\":[{\"position\":{\"x\":0.5,\"y\":-1.0},\"angle\":1.0,"
        "\"linear_velocity\":{\"x\":3.0,\"y\":4.0},\"angular_velocity\":2.0},null,"
        "{\"position\":{\"x\":-0.25,\"y\":0.125}}]}";
    Json log;
    // End 1 (side 1) and shot 1: team 1, index 0 -> sid 8; the stored stones keep positions only.
    CHECK(parse(shot_text("g", 1, 1, storage, "{\"velocity\":{\"x\":-0.5,\"y\":1.5},\"rotation\":\"cw\"}"), log));
    CapiInitial initial = build_capi_initial(log["log"]);
    CHECK_EQ(initial.delivered_sid, 8);
    CHECK(stone_float(initial.coord, 0, 0) == -0.5f);
    CHECK(stone_float(initial.coord, 0, 1) == static_cast<float>(1.0 + 21.0315));
    CHECK(stone_float(initial.coord, 0, 2) == 0.0f && stone_float(initial.coord, 0, 3) == 0.0f);
    CHECK(stone_float(initial.coord, 2, 0) == 0.25f);
    CHECK(stone_float(initial.coord, 2, 1) == static_cast<float>(-0.125 + 21.0315));
    CHECK(stone_float(initial.coord, 8, 3) == -0.5f && stone_float(initial.coord, 8, 4) == 1.5f);
    CHECK(stone_float(initial.coord, 8, 5) == static_cast<float>(-3.141592653589793 / 2.0));
    // The delivered stone comes from the first non-empty trajectory row.
    CHECK(parse(shot_text("g", 0, 1, storage + ",\"trajectory\":{\"frames\":[[null,{\"team\":\"team1\",\"index\":3,"
                                                "\"value\":{}}]]}"),
                log));
    CHECK_EQ(build_capi_initial(log["log"]).delivered_sid, 11);
    // shot == 0 ignores the storage.
    CHECK(parse(shot_text("g", 0, 0, storage), log));
    CHECK(stone_float(build_capi_initial(log["log"]).coord, 2, 0) == 0.0f);
    CHECK(parse("{\"end\":0,\"shot\":3,\"simulator_storage\":{\"stones\":[{}]}}", log));
    CHECK_THROWS(build_capi_initial(log));
}

TEST(prepared_binary_round_trip) {
    PreparedRecord record;
    record.index = 7;
    record.width = 4.75f;
    for (std::size_t i = 0; i < kCoordSize; ++i) record.coord[i] = static_cast<std::uint8_t>(i * 7);
    std::string data = encode_prepared({record, record});
    CHECK_EQ(data.size(), kPreparedHeaderSize + 2 * kRecordSize);
    CHECK_EQ(data.compare(0, 8, "DCSTIN01"), 0);
    CHECK_EQ(static_cast<unsigned char>(data[8]), static_cast<unsigned char>(2));
    std::vector<PreparedRecord> back = decode_prepared(data);
    CHECK_EQ(back.size(), static_cast<std::size_t>(2));
    CHECK(back[1].index == 7 && back[1].width == 4.75f && back[1].coord == record.coord);
    CHECK_THROWS(decode_prepared(data + "x"));
    CHECK_THROWS(decode_prepared("DCSTIN02" + data.substr(8)));
    CHECK_THROWS(encode_record(0, 1e300, record.coord));
}

TEST(catalogue_of_plain_files) {
    testing::TempDir temp;
    fs::path root = resolve(temp.path());
    write_file_new(root / "b" / "shot_e000s01.json", shot_text("game-b", 0, 1));
    write_file_new(root / "b" / "shot_e000s00.json", shot_text("game-b", 0, 0));
    write_file_new(root / "b" / "config.json", kConfig);
    write_file_new(root / "a" / "shot_e000s00.json", shot_text("", 0, 0));  // no game_id: path:a
    write_file_new(root / "a" / "shot_e000s02.json",
                   shot_text("game-b", 0, 2, std::string(), "{\"type\":\"concede\"}"));
    write_file_new(root / "a" / "game.dcl2",
                   "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":0,\"shot\":1},"
                   "\"last_move\":{\"free_guard_zone_foul\":false}}}\n");
    write_file_new(root / "a" / "other.txt", "ignored");

    Catalogue catalogue(root, 0);
    CHECK_EQ(catalogue.count(), static_cast<std::size_t>(3));
    CHECK_EQ(catalogue.skipped(), static_cast<std::int64_t>(1));
    CHECK_EQ(catalogue.input_digest(), std::string("shot-manifest-v1"));
    CHECK_EQ(catalogue.input_sha256().size(), static_cast<std::size_t>(64));
    // Order: (game, end, shot) with game ids compared as bytes: "game-b" < "path:a".
    CHECK_EQ(catalogue.rows()[0].shot_id, std::string("game-b/end:0/shot:0"));
    CHECK_EQ(catalogue.rows()[1].shot_id, std::string("game-b/end:0/shot:1"));
    CHECK_EQ(catalogue.rows()[2].shot_id, std::string("path:a/end:0/shot:0"));
    CHECK(catalogue.rows()[0].width == 4.5 && catalogue.rows()[0].width_source == "config");
    CHECK(catalogue.rows()[2].width == 4.75 && catalogue.rows()[2].width_source == "fallback");
    CHECK_EQ(catalogue.rows()[0].fgz, std::string("unknown"));
    CHECK_EQ(catalogue.rows()[2].index, static_cast<std::int64_t>(2));

    // limit counts discovered files (the concede shot included); discovery order is sorted paths.
    Catalogue limited(root, 2);
    CHECK_EQ(limited.count() + static_cast<std::size_t>(limited.skipped()), static_cast<std::size_t>(2));
    CHECK_EQ(limited.rows()[0].shot_id, std::string("path:a/end:0/shot:0"));
    CHECK(limited.input_sha256() != catalogue.input_sha256());

    ShotSource source(catalogue);
    LoadedShot loaded = source.load(source.locate(1));
    CHECK_EQ(loaded.shot_id, std::string("game-b/end:0/shot:1"));
    CHECK_EQ(loaded.log["shot"].get<int>(), 1);
    CHECK_THROWS(source.locate(3));
    // A shot file that changes after cataloguing is rejected when it is loaded.
    fs::remove(root / "b" / "shot_e000s01.json");
    write_file_new(root / "b" / "shot_e000s01.json", shot_text("game-b", 0, 1, ",\"x\":1"));
    CHECK_THROWS(source.load(source.locate(1)));
}

TEST(catalogue_of_zip_files_and_errors) {
    testing::TempDir temp;
    fs::path root = resolve(temp.path());
    {
        ZipWriter zip(root / "g1.zip");
        zip.add("config.json", kConfig);
        zip.add("game.dcl2",
                "{\"log\":{\"cmd\":\"update\",\"state\":{\"end\":0,\"shot\":2},"
                "\"last_move\":{\"free_guard_zone_foul\":true}}}\n");
        zip.add("shot_e000s01.json", shot_text("zgame", 0, 1));
        zip.add("shot_e000s00.json", shot_text("zgame", 0, 0));
        zip.add("notes.txt", "x");
        zip.finish();
    }
    Catalogue catalogue(root, 0);
    CHECK_EQ(catalogue.count(), static_cast<std::size_t>(2));
    CHECK_EQ(catalogue.rows()[0].shot_id, std::string("zgame/end:0/shot:0"));
    CHECK_EQ(catalogue.rows()[1].fgz, std::string("rule_restore"));
    CHECK(catalogue.rows()[0].width == 4.5);
    CHECK_EQ(catalogue.rows()[0].ref.member, std::string("shot_e000s00.json"));

    // A second archive with the same shot ids is a duplicate.
    {
        ZipWriter zip(root / "g2.zip");
        zip.add("shot_e000s00.json", shot_text("zgame", 0, 0));
        zip.finish();
    }
    CHECK_THROWS(Catalogue(root, 0));
    try {
        Catalogue again(root, 0);
    } catch (const DcDeterminismError& error) {
        CHECK(std::string(error.what()).find("duplicate shot ID zgame/end:0/shot:0") == 0);
    }
    // Empty input.
    testing::TempDir empty;
    CHECK_THROWS(Catalogue(resolve(empty.path()), 0));
}

TEST(fallback_game_id_forms) {
    fs::path root = fs::path("logs");
    CHECK_EQ(fallback_game_id(ShotRef{"", (root / "x" / "y" / "shot_e0s0.json").string()}, root), std::string("path:x/y"));
    CHECK_EQ(fallback_game_id(ShotRef{(root / "d" / "g.zip").string(), "sub/shot_e0s0.json"}, root),
             std::string("path:d/g/sub"));
    CHECK_EQ(fallback_game_id(ShotRef{(root / "g.zip").string(), "shot_e0s0.json"}, root), std::string("path:g"));
}

TEST(capture_shots_accepts_the_compare_report_schemas) {
    testing::TempDir temp;
    fs::path logs = resolve(temp.path()) / "logs";
    {
        ZipWriter zip(logs / "g1.zip");
        zip.add("config.json", kConfig);
        for (int shot = 0; shot < 2; ++shot) {
            zip.add("shot_e000s0" + std::to_string(shot) + ".json", shot_text("zgame", 0, shot));
        }
        zip.finish();
    }
    fs::path prepared_dir = resolve(temp.path()) / "prepared";
    Json identity = Json::object();
    identity["implementation"] = "cpp";
    Json document = prepare_corpus(logs, 0, prepared_dir, identity);

    auto capture = [&](const fs::path& report, const std::string& name) {
        std::string error;
        try {
            std::unique_ptr<cli::Parser> parser = cli::build_parser();
            cli::ParsedArgs args = parser->parse({"capture", "simulator", "--prepared", path_text(prepared_dir),
                                                  "--engine", "fcv1", "--compiler", "gcc", "--shots",
                                                  path_text(report), "--output",
                                                  path_text(resolve(temp.path()) / (name + ".zip"))});
            cli::dispatch(args);
        } catch (const DcDeterminismError& exception) {
            error = exception.what();
        }
        return error;
    };
    for (const std::string schema : {"dc-determinism-comparison-v2", "dc-determinism-capture-comparison-v2"}) {
        Json report = Json::object();
        report["schema"] = schema;
        Json contract = Json::object();
        contract["observation"] = "raw-stonecoordinate-v1";
        contract["record_digest"] = document["record_digest"];
        contract["corpus_input_sha256"] = document["corpus"]["input_sha256"];
        report["contract"] = contract;
        for (const char* mode : {"trajectory", "final"}) {
            report[mode] = Json::object({{"mismatch_shots", Json::array()}});
        }
        fs::path file = resolve(temp.path()) / (schema + ".json");
        write_file_new(file, json_dumps(report));
        // Past the schema check: it may still fail later (loader and worker resolution), but not on the schema.
        std::string error = capture(file, schema);
        CHECK(error.find("is not a `compare simulator` report") == std::string::npos);
    }
    fs::path unknown = resolve(temp.path()) / "unknown.json";
    write_file_new(unknown, "{\"schema\": \"dc-determinism-comparison-v3\"}");
    CHECK_EQ(capture(unknown, "unknown"),
             path_text(unknown) +
                 " is not a `compare simulator` report (dc-determinism-comparison-v2 or dc-determinism-capture-comparison-v2)");
}

TEST(prepared_directories_with_another_schema_name_are_refused) {
    testing::TempDir temp;
    fs::path logs = resolve(temp.path()) / "logs";
    {
        ZipWriter zip(logs / "g1.zip");
        zip.add("config.json", kConfig);
        for (int shot = 0; shot < 2; ++shot) {
            zip.add("shot_e000s0" + std::to_string(shot) + ".json", shot_text("zgame", 0, shot));
        }
        zip.finish();
    }
    fs::path output = resolve(temp.path()) / "prepared";
    Json identity = Json::object();
    identity["implementation"] = "cpp";
    Json document = prepare_corpus(logs, 0, output, identity);
    CHECK_EQ(document["schema"], Json("dc-determinism-prepared-v1"));  // written with the current name only
    fs::path other = resolve(temp.path()) / "other";
    write_file_new(other / "prepared.bin", read_file(output / "prepared.bin"));
    Json changed = read_json(output / "prepared.json");
    changed["schema"] = "dc-determinism-prepared-v2";
    write_file_new(other / "prepared.json", json_dumps(changed));
    CHECK_THROWS(load_prepared(other));
}

TEST(prepare_and_rebuild_catalogue) {
    testing::TempDir temp;
    fs::path logs = resolve(temp.path()) / "logs";
    {
        ZipWriter zip(logs / "g1.zip");
        zip.add("config.json", kConfig);
        for (int shot = 0; shot < 4; ++shot) {
            zip.add("shot_e000s0" + std::to_string(shot) + ".json", shot_text("zgame", 0, shot));
        }
        zip.finish();
    }
    fs::path output = resolve(temp.path()) / "out";
    Json identity = Json::object();
    identity["implementation"] = "cpp";
    std::unique_ptr<Catalogue> kept;
    Json document = prepare_corpus(logs, 0, output, identity, &kept);
    CHECK_EQ(document["corpus"]["count"].get<int>(), 4);
    CHECK_EQ(document["corpus"]["limit"].get<int>(), 0);
    CHECK_EQ(document["preparer"]["dc_determinism"]["implementation"].get<std::string>(), std::string("cpp"));
    CHECK(kept != nullptr && kept->count() == 4);
    CHECK_EQ(fs::file_size(output / "prepared.bin"), kPreparedHeaderSize + 4 * kRecordSize);
    CHECK_THROWS(prepare_corpus(logs, 0, output, identity));  // never overwrites

    PreparedCorpus prepared = load_prepared(output);
    CHECK_EQ(recorded_limit(prepared.document["corpus"]), static_cast<std::int64_t>(0));
    std::unique_ptr<Catalogue> rebuilt = rebuild_catalogue(logs, prepared);
    CHECK_EQ(rebuilt->input_sha256(), kept->input_sha256());

    // Old prepared directories record no limit: count + skipped_concede selects the same files.
    Json corpus = Json::object();
    corpus["count"] = 4;
    corpus["skipped_concede"] = 1;
    CHECK_EQ(recorded_limit(corpus), static_cast<std::int64_t>(5));
    CHECK_THROWS(recorded_limit(Json::object()));

    // A prepared directory that does not match its logs is refused.
    testing::TempDir other;
    fs::path other_logs = resolve(other.path()) / "logs";
    {
        ZipWriter zip(other_logs / "g1.zip");
        for (int shot = 0; shot < 4; ++shot) {
            zip.add("shot_e000s0" + std::to_string(shot) + ".json", shot_text("zgame", 0, shot));
        }
        zip.finish();  // no config.json: other sheet width -> other manifest digest
    }
    CHECK_THROWS(rebuild_catalogue(other_logs, prepared));
    CHECK_THROWS(rebuild_catalogue(resolve(temp.path()) / "missing", prepared));

    // A corrupted prepared.bin is detected.
    std::string bin = read_file(output / "prepared.bin");
    fs::path broken = resolve(temp.path()) / "broken";
    bin[kPreparedHeaderSize + 20] = static_cast<char>(bin[kPreparedHeaderSize + 20] ^ 1);
    write_file_new(broken / "prepared.bin", bin);
    write_file_new(broken / "prepared.json", read_file(output / "prepared.json"));
    CHECK_THROWS(load_prepared(broken));
    CHECK_THROWS(load_prepared(resolve(temp.path()) / "nothing"));
}

int main() { return testing::run_all_tests(); }
