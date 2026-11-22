#include <string>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "tempdir.hpp"
#include "testing.hpp"
#include "zip/zip.hpp"

using dc_determinism::deflate_member;
using dc_determinism::ZipReader;
using dc_determinism::ZipWriter;

namespace {

std::string pseudo_random(std::size_t size, unsigned seed) {
    std::string out(size, '\0');
    unsigned state = seed;
    for (char& c : out) {
        state = state * 1664525u + 1013904223u;
        c = static_cast<char>(state >> 24);
    }
    return out;
}

}  // namespace

TEST(round_trip) {
    testing::TempDir dir;
    std::filesystem::path archive = dir / "a.zip";
    std::string text(100000, 'x');
    for (std::size_t i = 0; i < text.size(); i += 7) text[i] = static_cast<char>('a' + i % 26);
    std::string noise = pseudo_random(65537, 7);
    {
        ZipWriter writer(archive);
        writer.add("shots/0.dctr", text);
        writer.add("index.json", "{\"a\": 1}\n");
        writer.add("empty", "");
        writer.add("noise.bin", noise);
        writer.add("stored.txt", "stored", false);
        writer.add("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.txt", "unicode name");
        writer.finish();
    }
    ZipReader reader(archive);
    CHECK_EQ(reader.entries().size(), static_cast<std::size_t>(6));
    CHECK_EQ(reader.read("shots/0.dctr"), text);
    CHECK_EQ(reader.read("index.json"), std::string("{\"a\": 1}\n"));
    CHECK_EQ(reader.read("empty"), std::string());
    CHECK_EQ(reader.read("noise.bin"), noise);
    CHECK_EQ(reader.read("stored.txt"), std::string("stored"));
    CHECK_EQ(reader.read("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.txt"), std::string("unicode name"));
    CHECK_EQ(reader.find("missing"), -1);
    CHECK(reader.entries()[0].compressed_size < reader.entries()[0].size);
    CHECK_THROWS(reader.read("missing"));
}

TEST(precompressed_members) {
    testing::TempDir dir;
    std::filesystem::path archive = dir / "a.zip";
    std::string text(50000, 'y');
    for (std::size_t i = 0; i < text.size(); i += 5) text[i] = static_cast<char>('a' + i % 23);
    std::string noise = pseudo_random(30001, 11);
    {
        ZipWriter writer(archive);
        writer.add_deflated("text", deflate_member(text));
        writer.add_deflated("empty", deflate_member(""));
        writer.add_deflated("tiny", deflate_member("ab"));
        writer.add_deflated("noise", deflate_member(noise));
        writer.add("plain", "mixed with ordinary members");
        writer.finish();
    }
    ZipReader reader(archive);
    CHECK_EQ(reader.entries().size(), static_cast<std::size_t>(5));
    CHECK_EQ(reader.read("text"), text);
    CHECK_EQ(reader.read("empty"), std::string());
    CHECK_EQ(reader.read("tiny"), std::string("ab"));
    CHECK_EQ(reader.read("noise"), noise);
    CHECK_EQ(reader.read("plain"), std::string("mixed with ordinary members"));
    CHECK(reader.entries()[0].compressed_size < reader.entries()[0].size);
}

TEST(writer_never_overwrites) {
    testing::TempDir dir;
    std::filesystem::path archive = dir / "a.zip";
    {
        ZipWriter writer(archive);
        writer.add("x", "1");
        writer.finish();
    }
    bool refused = false;
    try {
        ZipWriter second(archive);
    } catch (const dc_determinism::DcDeterminismError&) {
        refused = true;
    }
    CHECK(refused);
    // the existing archive is untouched
    ZipReader reader(archive);
    CHECK_EQ(reader.read("x"), std::string("1"));
}

TEST(unfinished_writer_leaves_nothing) {
    testing::TempDir dir;
    std::filesystem::path archive = dir / "a.zip";
    {
        ZipWriter writer(archive);
        writer.add("x", "1");
    }
    CHECK(!std::filesystem::exists(archive));
    CHECK(std::filesystem::is_empty(dir.path()));
}

TEST(not_a_zip) {
    testing::TempDir dir;
    dc_determinism::write_file_new(dir / "x.zip", "this is not a zip file");
    CHECK_THROWS(ZipReader(dir / "x.zip"));
    CHECK_THROWS(ZipReader(dir / "missing.zip"));
}

int main() { return testing::run_all_tests(); }
