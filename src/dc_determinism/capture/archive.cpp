#include "capture/archive.hpp"

#include "common/error.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"

namespace dc_determinism {

const char* const kCaptureSchema = "dc-determinism-capture-v1";
const char* const kCaptureIndexName = "index.json";

namespace {

const char* const kObservation = "raw-stonecoordinate-v1";
const char* const kContractKeys[] = {"engine", "observation", "max_steps", "prepared_sha256", "record_digest",
                                     "corpus_input_sha256"};

void need(bool condition, const std::string& path, const std::string& message) {
    if (!condition) throw DcDeterminismError("invalid capture archive " + path + ": " + message);
}

}  // namespace

std::string capture_member_name(std::int64_t index) { return "shots/" + std::to_string(index) + ".dctr"; }

// ---------------------------------------------------------------------------------- writer

CaptureWriter::CaptureWriter(const fs::path& final_path) : zip_(final_path) {}

void CaptureWriter::add(const std::string& name, const std::string& bytes) {
    DeflatedMember member = deflate_member(bytes);  // outside the lock: the workers compress in parallel
    std::lock_guard<std::mutex> lock(mutex_);
    zip_.add_deflated(name, member);
}

void CaptureWriter::commit(const Json& index) {
    std::lock_guard<std::mutex> lock(mutex_);
    zip_.add(kCaptureIndexName, json_pretty_file_text(index), true);
    zip_.finish();
}

// ---------------------------------------------------------------------------------- reader

CaptureArchive::CaptureArchive(const std::string& path) : path_(path) {
    std::unique_ptr<ZipReader> zip;
    try {
        zip.reset(new ZipReader(path_from_utf8(path)));
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("cannot open capture archive " + path + ": " + exc.what());
    }
    const std::vector<ZipEntry>& entries = zip->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) members_[entries[i].name] = i;  // the last entry wins
    auto found = members_.find(kCaptureIndexName);
    need(found != members_.end(), path, std::string("no ") + kCaptureIndexName);

    std::string text;
    try {
        text = zip->read(found->second);
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("invalid capture archive " + path + ": bad " + kCaptureIndexName + ": " + exc.what());
    }
    std::string error;
    if (!is_valid_utf8(text)) {
        throw DcDeterminismError("invalid capture archive " + path + ": bad " + kCaptureIndexName +
                         ": 'utf-8' codec can't decode the index");
    }
    if (!py_json_loads(text, index_, error)) {
        throw DcDeterminismError("invalid capture archive " + path + ": bad " + kCaptureIndexName + ": " + error);
    }
    need(index_.is_object() && index_.contains("schema") && index_["schema"] == kCaptureSchema, path,
         std::string("schema is not ") + kCaptureSchema);
    bool complete = index_.contains("contract") && index_["contract"].is_object();
    for (const char* key : kContractKeys) complete = complete && index_["contract"].contains(key);
    need(complete, path, "incomplete contract");
    need(index_["contract"]["observation"] == kObservation, path, "unknown observation");
    need(index_.contains("format") && index_["format"] == kDctrFormat, path,
         std::string("format is not ") + kDctrFormat);
    need(index_.contains("shots") && index_["shots"].is_array(), path, "no shot list");
    const Json& shots = index_["shots"];
    for (std::size_t i = 0; i < shots.size(); ++i) {
        const Json& row = shots[i];
        bool valid = row.is_object() && row.contains("shot_id") && row["shot_id"].is_string() &&
                     row.contains("index") && row["index"].is_number_integer() && row.contains("steps") &&
                     row["steps"].is_number_integer() && row.contains("status") &&
                     (row["status"] == "stopped" || row["status"] == "truncated") && row.contains("member") &&
                     row["member"].is_string();
        need(valid, path, "malformed shot entry");
        std::string id = row["shot_id"].get<std::string>();
        need(by_id_.count(id) == 0, path, "duplicate shot_id " + id);
        need(members_.count(row["member"].get<std::string>()) != 0, path,
             "missing member " + row["member"].get<std::string>());
        by_id_[id] = i;
        ids_.push_back(id);
    }
}

const Json& CaptureArchive::row(const std::string& shot_id) const { return index_["shots"][by_id_.at(shot_id)]; }

std::unique_ptr<ZipReader> CaptureArchive::open_reader() const {
    try {
        return std::unique_ptr<ZipReader>(new ZipReader(path_from_utf8(path_)));
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("cannot open capture archive " + path_ + ": " + exc.what());
    }
}

CapturedTrajectory CaptureArchive::trajectory(const std::string& shot_id, const ZipReader& reader) const {
    std::string member = row(shot_id)["member"].get<std::string>();
    std::string payload;
    try {
        payload = reader.read(members_.at(member));
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("cannot read " + member + " in " + path_ + ": " + exc.what());
    }
    return CapturedTrajectory(std::move(payload));
}

}  // namespace dc_determinism
