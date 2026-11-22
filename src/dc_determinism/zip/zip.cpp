#include "zip/zip.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#include <miniz.h>

#include "common/error.hpp"
#include "common/fs.hpp"

namespace dc_determinism {

namespace {

std::FILE* open_binary(const fs::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const char* c = mode; *c != '\0'; ++c) wide_mode.push_back(static_cast<wchar_t>(*c));
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::string zip_error(mz_zip_archive& archive) {
    return mz_zip_get_error_string(mz_zip_get_last_error(&archive));
}

}  // namespace

// ---------------------------------------------------------------------------------- reader

struct ZipReader::Impl {
    mz_zip_archive archive;
    std::FILE* file = nullptr;
    bool open = false;
};

ZipReader::ZipReader(const fs::path& path) : impl_(new Impl), path_(path) {
    std::memset(&impl_->archive, 0, sizeof(impl_->archive));
    impl_->file = open_binary(path, "rb");
    if (impl_->file == nullptr) {
        throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": " + std::strerror(errno));
    }
    if (std::fseek(impl_->file, 0, SEEK_END) != 0) {
        std::fclose(impl_->file);
        throw DcDeterminismError("cannot read " + path_to_utf8(path) + ": seek failed");
    }
#ifdef _WIN32
    long long size = _ftelli64(impl_->file);
#else
    long long size = ftello(impl_->file);
#endif
    std::rewind(impl_->file);
    if (size < 0 || !mz_zip_reader_init_cfile(&impl_->archive, impl_->file,
                                              static_cast<mz_uint64>(size), 0)) {
        std::string reason = zip_error(impl_->archive);
        std::fclose(impl_->file);
        throw DcDeterminismError("not a ZIP file: " + path_to_utf8(path) + " (" + reason + ")");
    }
    impl_->open = true;
    mz_uint count = mz_zip_reader_get_num_files(&impl_->archive);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&impl_->archive, i, &stat)) {
            throw DcDeterminismError("corrupt ZIP directory in " + path_to_utf8(path_) + ": " +
                             zip_error(impl_->archive));
        }
        ZipEntry entry;
        entry.name = stat.m_filename;
        entry.size = stat.m_uncomp_size;
        entry.compressed_size = stat.m_comp_size;
        entry.crc32 = static_cast<std::uint32_t>(stat.m_crc32);
        entry.is_directory = stat.m_is_directory != 0;
        entry.encrypted = (stat.m_bit_flag & 1u) != 0;
        entries_.push_back(entry);
    }
}

ZipReader::~ZipReader() {
    if (impl_->open) mz_zip_reader_end(&impl_->archive);
    if (impl_->file != nullptr) std::fclose(impl_->file);
}

int ZipReader::find(const std::string& name) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

std::string ZipReader::read(std::size_t index) const {
    if (index >= entries_.size()) throw DcDeterminismError("ZIP member index out of range");
    const ZipEntry& entry = entries_[index];
    std::string out(static_cast<std::size_t>(entry.size), '\0');
    if (entry.size == 0) return out;
    // mz_zip_reader_extract_to_mem checks the CRC-32 of the decompressed bytes.
    if (!mz_zip_reader_extract_to_mem(&impl_->archive, static_cast<mz_uint>(index), &out[0], out.size(),
                                      0)) {
        throw DcDeterminismError("cannot extract " + entry.name + " from " + path_to_utf8(path_) + ": " +
                         zip_error(impl_->archive));
    }
    return out;
}

std::string ZipReader::read(const std::string& name) const {
    int index = find(name);
    if (index < 0) throw DcDeterminismError("no member " + name + " in " + path_to_utf8(path_));
    return read(static_cast<std::size_t>(index));
}

// ---------------------------------------------------------------------------------- writer

struct ZipWriter::Impl {
    mz_zip_archive archive;
    std::FILE* file = nullptr;
    fs::path temporary;
    bool open = false;
    bool finished = false;
};

ZipWriter::ZipWriter(const fs::path& path) : impl_(new Impl), path_(path) {
    std::memset(&impl_->archive, 0, sizeof(impl_->archive));
    std::error_code ec;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    refuse_existing(path);
    impl_->temporary = create_temporary_sibling(path);
    impl_->file = open_binary(impl_->temporary, "wb");
    if (impl_->file == nullptr) {
        int code = errno;
        fs::remove(impl_->temporary, ec);
        throw DcDeterminismError("cannot write " + path_to_utf8(impl_->temporary) + ": " + std::strerror(code));
    }
    if (!mz_zip_writer_init_cfile(&impl_->archive, impl_->file, 0)) {
        std::string reason = zip_error(impl_->archive);
        std::fclose(impl_->file);
        impl_->file = nullptr;
        fs::remove(impl_->temporary, ec);
        throw DcDeterminismError("cannot start the ZIP archive " + path_to_utf8(path) + ": " + reason);
    }
    impl_->open = true;
}

ZipWriter::~ZipWriter() {
    if (impl_->open) mz_zip_writer_end(&impl_->archive);
    if (impl_->file != nullptr) std::fclose(impl_->file);
    if (!impl_->finished) {
        std::error_code ec;
        fs::remove(impl_->temporary, ec);
    }
}

void ZipWriter::add(const std::string& name, const std::string& bytes, bool deflate) {
    if (impl_->finished) throw DcDeterminismError("the ZIP archive is already finished");
    mz_uint flags = deflate ? 6 : MZ_NO_COMPRESSION;
    if (!mz_zip_writer_add_mem(&impl_->archive, name.c_str(), bytes.data(), bytes.size(), flags)) {
        throw DcDeterminismError("cannot add " + name + " to " + path_to_utf8(path_) + ": " +
                         zip_error(impl_->archive));
    }
}

DeflatedMember deflate_member(const std::string& bytes) {
    DeflatedMember out;
    out.size = bytes.size();
    out.crc32 = static_cast<std::uint32_t>(
        mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()));
    // Window bits -15: a raw deflate stream, as a ZIP member stores it.
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY);
    std::size_t length = 0;
    void* heap = tdefl_compress_mem_to_heap(bytes.data(), bytes.size(), &length, static_cast<int>(flags));
    if (heap == nullptr) throw DcDeterminismError("cannot compress a ZIP member");
    out.data.assign(static_cast<const char*>(heap), length);
    mz_free(heap);
    return out;
}

void ZipWriter::add_deflated(const std::string& name, const DeflatedMember& member) {
    if (impl_->finished) throw DcDeterminismError("the ZIP archive is already finished");
    if (!mz_zip_writer_add_mem_ex(&impl_->archive, name.c_str(), member.data.data(), member.data.size(), nullptr, 0,
                                  6 | MZ_ZIP_FLAG_COMPRESSED_DATA, member.size, member.crc32)) {
        throw DcDeterminismError("cannot add " + name + " to " + path_to_utf8(path_) + ": " + zip_error(impl_->archive));
    }
}

void ZipWriter::finish() {
    if (impl_->finished) return;
    if (!mz_zip_writer_finalize_archive(&impl_->archive)) {
        throw DcDeterminismError("cannot finalize " + path_to_utf8(path_) + ": " + zip_error(impl_->archive));
    }
    mz_zip_writer_end(&impl_->archive);
    impl_->open = false;
    std::fflush(impl_->file);
    std::fclose(impl_->file);
    impl_->file = nullptr;
    commit_new(impl_->temporary, path_);
    impl_->finished = true;
}

}  // namespace dc_determinism
