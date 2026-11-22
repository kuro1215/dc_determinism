// JSON writer semantics against CPython json.dumps (expected strings produced with CPython 3.14).
#include <cstdint>
#include <limits>

#include "common/error.hpp"
#include "json/writer.hpp"
#include "testing.hpp"

using dc_determinism::DumpOptions;
using dc_determinism::Json;
using dc_determinism::json_dumps;

namespace {

Json sample() {
    Json value = Json::object();
    value["a"] = 1;
    Json list = Json::array();
    list.push_back(1.5);
    list.push_back(2.0);
    list.push_back(1e-05);
    list.push_back(nullptr);
    list.push_back(true);
    list.push_back(false);
    list.push_back(-0.0);
    list.push_back(1e16);
    list.push_back(static_cast<std::uint64_t>(12345678901234567890ULL));
    value["b"] = list;
    value["c"] = Json::object();
    value["d"] = Json::array();
    value["e"] = std::string("x\"y\\z\n\t\xc3\xa9\xf0\x9f\x98\x80\x7f\x01");
    Json inner = Json::object();
    inner["y"] = Json::array();
    Json deep = Json::object();
    deep["w"] = 1;
    inner["x"] = deep;
    value["z"] = inner;
    return value;
}

}  // namespace

TEST(default_separators) {
    CHECK_EQ(json_dumps(sample()),
             std::string("{\"a\": 1, \"b\": [1.5, 2.0, 1e-05, null, true, false, -0.0, 1e+16, "
                         "12345678901234567890], \"c\": {}, \"d\": [], \"e\": "
                         "\"x\\\"y\\\\z\\n\\t\\u00e9\\ud83d\\ude00\\u007f\\u0001\", \"z\": {\"y\": [], "
                         "\"x\": {\"w\": 1}}}"));
}

TEST(indent_two) {
    CHECK_EQ(json_dumps(sample(), DumpOptions::pretty(2, false)),
             std::string("{\n  \"a\": 1,\n  \"b\": [\n    1.5,\n    2.0,\n    1e-05,\n    null,\n    true,\n"
                         "    false,\n    -0.0,\n    1e+16,\n    12345678901234567890\n  ],\n  \"c\": {},\n"
                         "  \"d\": [],\n  \"e\": \"x\\\"y\\\\z\\n\\t\\u00e9\\ud83d\\ude00\\u007f\\u0001\",\n"
                         "  \"z\": {\n    \"y\": [],\n    \"x\": {\n      \"w\": 1\n    }\n  }\n}"));
}

TEST(compact_sorted) {
    CHECK_EQ(json_dumps(sample(), DumpOptions::compact(true)),
             std::string("{\"a\":1,\"b\":[1.5,2.0,1e-05,null,true,false,-0.0,1e+16,12345678901234567890],"
                         "\"c\":{},\"d\":[],\"e\":\"x\\\"y\\\\z\\n\\t\\u00e9\\ud83d\\ude00\\u007f\\u0001\","
                         "\"z\":{\"x\":{\"w\":1},\"y\":[]}}"));
}

TEST(ensure_ascii_off) {
    DumpOptions options;
    options.ensure_ascii = false;
    CHECK_EQ(json_dumps(sample(), options),
             std::string("{\"a\": 1, \"b\": [1.5, 2.0, 1e-05, null, true, false, -0.0, 1e+16, "
                         "12345678901234567890], \"c\": {}, \"d\": [], \"e\": "
                         "\"x\\\"y\\\\z\\n\\t\xc3\xa9\xf0\x9f\x98\x80\x7f\\u0001\", \"z\": {\"y\": [], "
                         "\"x\": {\"w\": 1}}}"));
}

TEST(non_finite_floats) {
    Json list = Json::array();
    list.push_back(std::numeric_limits<double>::quiet_NaN());
    list.push_back(std::numeric_limits<double>::infinity());
    list.push_back(-std::numeric_limits<double>::infinity());
    CHECK_EQ(json_dumps(list), std::string("[NaN, Infinity, -Infinity]"));
    DumpOptions strict;
    strict.allow_nan = false;
    CHECK_THROWS(json_dumps(list, strict));
}

TEST(integers_and_floats_stay_distinct) {
    Json parsed = dc_determinism::parse_json_text("[1, 1.0, -0, 1e2, 18446744073709551615, -9223372036854775808]", "test");
    CHECK_EQ(json_dumps(parsed), std::string("[1, 1.0, 0, 100.0, 18446744073709551615, -9223372036854775808]"));
}

TEST(parse_keeps_key_order) {
    Json parsed = dc_determinism::parse_json_text("{\"b\": 1, \"a\": {\"d\": 2, \"c\": 3}}", "test");
    CHECK_EQ(json_dumps(parsed, DumpOptions::compact()), std::string("{\"b\":1,\"a\":{\"d\":2,\"c\":3}}"));
}

TEST(malformed_input_is_a_dc_determinism_error) {
    bool threw = false;
    try {
        dc_determinism::parse_json_text("{\"a\": ", "some.json");
    } catch (const dc_determinism::DcDeterminismError& error) {
        threw = std::string(error.what()).find("malformed JSON in some.json") == 0;
    }
    CHECK(threw);
}

int main() { return testing::run_all_tests(); }
