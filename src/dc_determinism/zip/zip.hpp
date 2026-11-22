// ZIP archives through the vendored miniz (src/vendor/miniz): reading the game logs and capture
// archives, writing capture archives. Member names are the raw bytes stored in the archive
// (UTF-8 for archives written by Python's zipfile and by this tool).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/platform.hpp"

namespace dc_determinism {

struct ZipEntry {
    std::string name;
    std::uint64_t size = 0;             // uncompressed
    std::uint64_t compressed_size = 0;
    std::uint32_t crc32 = 0;
    bool is_directory = false;
    bool encrypted = false;             // general purpose flag bit 0
};

class ZipReader {
public:
    // Failure: DcDeterminismError("cannot read <path>: ..." / "not a ZIP file: <path>").
    explicit ZipReader(const fs::path& path);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    const std::vector<ZipEntry>& entries() const { return entries_; }
    // Index of the first member called `name`, or -1.
    int find(const std::string& name) const;
    // Decompressed bytes (CRC checked); DcDeterminismError on a corrupt member.
    std::string read(std::size_t index) const;
    std::string read(const std::string& name) const;  // DcDeterminismError when missing

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::vector<ZipEntry> entries_;
    fs::path path_;
};

// A member compressed ahead of time (`deflate_member`): raw deflate data, CRC-32 and size of the
// uncompressed bytes.
struct DeflatedMember {
    std::string data;
    std::uint32_t crc32 = 0;
    std::uint64_t size = 0;
};

// Compresses `bytes` at level 6 like `ZipWriter::add(..., true)`; touches no archive, so several
// threads may compress while another one adds the finished members.
DeflatedMember deflate_member(const std::string& bytes);

class ZipWriter {
public:
    // The archive is built in a temporary sibling file and committed without ever replacing
    // an existing file by finish() ("refusing to overwrite existing output: ...").
    explicit ZipWriter(const fs::path& path);
    ~ZipWriter();  // removes the temporary file when finish() was not reached
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    // `deflate`: level 6 like Python's ZIP_DEFLATED default; false stores the bytes.
    void add(const std::string& name, const std::string& bytes, bool deflate = true);
    // Adds a member compressed by deflate_member() without compressing it again.
    void add_deflated(const std::string& name, const DeflatedMember& member);
    void finish();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    fs::path path_;
};

}  // namespace dc_determinism
