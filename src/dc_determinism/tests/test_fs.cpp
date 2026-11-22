#include <fstream>
#include <limits>
#include <optional>
#include <string>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/platform.hpp"
#include "common/sha256.hpp"
#include "common/tree_hash.hpp"
#include "json/writer.hpp"
#include "tempdir.hpp"
#include "testing.hpp"

using namespace dc_determinism;

namespace {

void write_raw(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

}  // namespace

TEST(write_file_new_refuses_existing) {
    testing::TempDir dir;
    fs::path target = dir / "out" / "a.txt";
    write_file_new(target, "first");
    CHECK_EQ(read_file(target), std::string("first"));
    bool refused = false;
    try {
        write_file_new(target, "second");
    } catch (const DcDeterminismError& error) {
        refused = std::string(error.what()).find("refusing to overwrite existing output") == 0;
    }
    CHECK(refused);
    CHECK_EQ(read_file(target), std::string("first"));
    // no temporary files are left behind
    int files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(dir / "out")) {
        (void)entry;
        ++files;
    }
    CHECK_EQ(files, 1);
}

TEST(commit_new_never_replaces) {
    testing::TempDir dir;
    write_raw(dir / "tmp1", "new");
    write_raw(dir / "final", "old");
    CHECK_THROWS(commit_new(dir / "tmp1", dir / "final"));
    CHECK_EQ(read_file(dir / "final"), std::string("old"));
    fs::remove(dir / "final");
    commit_new(dir / "tmp1", dir / "final");
    CHECK_EQ(read_file(dir / "final"), std::string("new"));
    CHECK(!fs::exists(dir / "tmp1"));
}

TEST(write_json_new_matches_python_layout) {
    testing::TempDir dir;
    Json value = Json::object();
    value["b"] = 1;
    value["a"] = Json::array({1.0, "x"});
    write_json_new(dir / "r.json", value);
    CHECK_EQ(read_file(dir / "r.json"), std::string("{\n  \"b\": 1,\n  \"a\": [\n    1.0,\n    \"x\"\n  ]\n}\n"));
    CHECK_THROWS(write_json_new(dir / "r.json", value));
    Json bad = Json::array({std::numeric_limits<double>::quiet_NaN()});
    CHECK_THROWS(write_json_new(dir / "nan.json", bad));
    CHECK(!fs::exists(dir / "nan.json"));
}

TEST(read_json_errors) {
    testing::TempDir dir;
    CHECK_THROWS(read_json(dir / "missing.json"));
    write_raw(dir / "bad.json", "{oops");
    CHECK_THROWS(read_json(dir / "bad.json"));
    write_raw(dir / "good.json", "{\"k\": [1, 2.5]}");
    CHECK_EQ(json_dumps(read_json(dir / "good.json")), std::string("{\"k\": [1, 2.5]}"));
}

TEST(sha256_of_files) {
    testing::TempDir dir;
    write_raw(dir / "abc", "abc");
    write_raw(dir / "crlf", "a\r\nb\r\n");
    CHECK_EQ(sha256_file(dir / "abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(sha256_lf_file(dir / "crlf"), sha256_hex("a\nb\n"));
    CHECK(sha256_file(dir / "crlf") != sha256_lf_file(dir / "crlf"));
}

TEST(find_repo_root_walks_up) {
    testing::TempDir dir;
    write_raw(dir / "repo" / "prebuilt" / "manifest.json", "{}");
    fs::create_directories(dir / "repo" / "src" / "vendor");
    fs::create_directories(dir / "repo" / "build" / "dc_determinism" / "linux-amd64");
    std::optional<fs::path> found = find_repo_root(dir / "repo" / "build" / "dc_determinism" / "linux-amd64");
    CHECK(found.has_value());
    if (found) CHECK_EQ(fs::path(*found).filename().string(), std::string("repo"));
    // src/vendor/ alone is not enough
    fs::create_directories(dir / "other" / "src" / "vendor" / "sub");
    CHECK(!find_repo_root(dir / "other" / "src" / "vendor" / "sub").has_value() ||
          find_repo_root(dir / "other" / "src" / "vendor" / "sub")->filename().string() != "other");
}

TEST(tree_hash_is_lf_normalised_and_ordered) {
    testing::TempDir dir;
    write_raw(dir / "t1" / "b.txt", "line\r\nline\r\n");
    write_raw(dir / "t1" / "a" / "c.txt", "x");
    write_raw(dir / "t2" / "b.txt", "line\nline\n");
    write_raw(dir / "t2" / "a" / "c.txt", "x");
    CHECK_EQ(tree_sha256_lf(dir / "t1"), tree_sha256_lf(dir / "t2"));
    write_raw(dir / "t2" / "a" / "c.txt", "y");
    CHECK(tree_sha256_lf(dir / "t1") != tree_sha256_lf(dir / "t2"));
}

TEST(vendored_dependencies_match_the_manifest) {
    // Every entry of src/vendor/manifest.json ("dependencies" and "dc_determinism_dependencies" with
    // miniz) and its digitalcurling entry must equal the tree hash computed here, i.e. the one the local
    // build verifies before every build.
    fs::path deps = fs::path(DC_DETERMINISM_REPO_ROOT_DIR) / "src" / "vendor";
    Json manifest = read_json(deps / "manifest.json");
    CHECK_EQ(manifest["schema"], Json("dc-determinism-vendor-v1"));
    CHECK_EQ(manifest["digitalcurling"]["tree_sha256_lf"].get<std::string>(),
             tree_sha256_lf(deps / "digitalcurling"));
    CHECK_EQ(manifest["digitalcurling"]["source"], Json("DigitalCurling"));
    CHECK_EQ(manifest["digitalcurling"]["license"], Json("MIT"));
    int checked = 0;
    for (const char* section : {"dependencies", "dc_determinism_dependencies"}) {
        for (auto it = manifest[section].begin(); it != manifest[section].end(); ++it) {
            std::string expected = it.value()["tree_sha256_lf"].get<std::string>();
            CHECK_EQ(tree_sha256_lf(deps / it.key()), expected);
            ++checked;
        }
    }
    CHECK_EQ(checked, 5);  // fpm, nlohmann-json, box2d, xxhash and miniz
    CHECK(manifest["dc_determinism_dependencies"].contains("miniz"));
    CHECK(!manifest["dependencies"].contains("miniz"));
    // One fpm tree for the loaders and the players (its include dir and LICENSE equal upstream's pin).
    CHECK_EQ(manifest["dependencies"]["fpm"]["role"], Json("loader+player"));
    CHECK_EQ(json_dumps(manifest["dependencies"]["fpm"]["overlay"], DumpOptions::compact()),
             std::string("[\"CMakeLists.txt\"]"));
    CHECK(fs::exists(deps / "fpm" / "CMakeLists.txt"));
    CHECK(!fs::exists(deps / "deps"));
}

int main() { return testing::run_all_tests(); }