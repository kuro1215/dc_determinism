// Capture archives (`dc-determinism-capture-v1`): a ZIP with `shots/<index>.dctr`
// members (dctr-v1, deflated) and `index.json` (written last).
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capture/dctr.hpp"
#include "common/platform.hpp"
#include "json/json.hpp"
#include "zip/zip.hpp"

namespace dc_determinism {

extern const char* const kCaptureSchema;     // "dc-determinism-capture-v1"
extern const char* const kCaptureIndexName;  // "index.json"

// `shots/<index>.dctr`
std::string capture_member_name(std::int64_t index);

// A new archive written to a temporary file and committed without ever replacing an existing file
// (`ArchiveWriter`). `add` may be called from several threads.
class CaptureWriter {
public:
    explicit CaptureWriter(const fs::path& final_path);

    void add(const std::string& name, const std::string& bytes);
    // Writes `index.json` last and commits the archive.
    void commit(const Json& index);

private:
    ZipWriter zip_;
    std::mutex mutex_;
};

// A validated `dc-determinism-capture-v1` archive. The parsed index is shared; `trajectory()` with a
// reader of the same file may run on several threads, each with its own ZipReader.
class CaptureArchive {
public:
    // DcDeterminismError "cannot open capture archive <path>: ..." / "invalid capture archive <path>: ...".
    explicit CaptureArchive(const std::string& path);

    const std::string& path() const { return path_; }
    const Json& index() const { return index_; }
    const Json& contract() const { return index_["contract"]; }
    // Shot ids in index order.
    const std::vector<std::string>& shot_ids() const { return ids_; }
    bool has(const std::string& shot_id) const { return by_id_.count(shot_id) != 0; }
    // The `shots` row of a shot id.
    const Json& row(const std::string& shot_id) const;

    // A reader of the archive file for another thread.
    std::unique_ptr<ZipReader> open_reader() const;
    CapturedTrajectory trajectory(const std::string& shot_id, const ZipReader& reader) const;

private:
    std::string path_;
    Json index_;
    std::vector<std::string> ids_;
    std::unordered_map<std::string, std::size_t> by_id_;  // position in index_["shots"]
    std::unordered_map<std::string, std::size_t> members_;  // member name -> entry number
};

}  // namespace dc_determinism
