#include "corpus/prepared.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/sha256.hpp"
#include "json/writer.hpp"

namespace dc_determinism {

const char* const kPreparedMagic = "DCSTIN01";
const char* const kPreparedSchema = "dc-determinism-prepared-v1";
const char* const kPreparedBinName = "prepared.bin";
const char* const kPreparedJsonName = "prepared.json";

namespace {

void append_u32(std::string& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
}

void append_float(std::string& out, float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(out, bits);
}

std::uint32_t read_u32(const std::string& data, std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + static_cast<std::size_t>(i)]))
                 << (8 * i);
    }
    return value;
}

bool is_hex64(const Json& value) {
    if (!value.is_string()) return false;
    const std::string& text = value.get_ref<const std::string&>();
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string seconds_text(double seconds) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.1f", seconds);
    return buffer;
}

double elapsed(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

std::string encode_record(std::uint32_t index, double width, const Coordinate& coord) {
    float narrow = static_cast<float>(width);
    if (std::isinf(narrow) && !std::isinf(width)) throw DcDeterminismError("float too large to pack with f format");
    std::string out;
    out.reserve(kRecordSize);
    append_u32(out, index);
    append_float(out, narrow);
    out.append(reinterpret_cast<const char*>(coord.data()), coord.size());
    return out;
}

std::string encode_prepared(const std::vector<PreparedRecord>& records) {
    std::string out(kPreparedMagic, 8);
    append_u32(out, static_cast<std::uint32_t>(records.size()));
    for (const PreparedRecord& record : records) out += encode_record(record.index, record.width, record.coord);
    return out;
}

std::vector<PreparedRecord> decode_prepared(const std::string& data) {
    if (data.size() < 8 || data.compare(0, 8, kPreparedMagic) != 0) throw DcDeterminismError("bad magic");
    if (data.size() < kPreparedHeaderSize) throw DcDeterminismError("size does not match the record count");
    std::uint32_t count = read_u32(data, 8);
    if (data.size() != kPreparedHeaderSize + kRecordSize * count) {
        throw DcDeterminismError("size does not match the record count");
    }
    std::vector<PreparedRecord> out(count);
    std::size_t offset = kPreparedHeaderSize;
    for (std::uint32_t i = 0; i < count; ++i) {
        out[i].index = read_u32(data, offset);
        std::uint32_t bits = read_u32(data, offset + 4);
        std::memcpy(&out[i].width, &bits, sizeof(bits));
        std::memcpy(out[i].coord.data(), data.data() + offset + kRecordHeadSize, kCoordSize);
        offset += kRecordSize;
    }
    return out;
}

PreparedCorpus load_prepared(const fs::path& directory) {
    fs::path bin_path = directory / kPreparedBinName;
    fs::path json_path = directory / kPreparedJsonName;
    std::error_code ec;
    if (!fs::is_regular_file(bin_path, ec) || !fs::is_regular_file(json_path, ec)) {
        throw DcDeterminismError(std::string("not a prepared directory (need ") + kPreparedBinName + " and " +
                         kPreparedJsonName + "): " + path_to_utf8(directory));
    }
    Json doc = read_json(json_path);
    std::string json_name = path_to_utf8(json_path);
    if (!doc.is_object() || !doc.contains("schema") || doc["schema"] != kPreparedSchema) {
        throw DcDeterminismError("unexpected prepared.json schema in " + json_name);
    }
    if (!doc.contains("shots") || !doc["shots"].is_array() || doc["shots"].empty() || !doc.contains("corpus") ||
        !doc["corpus"].is_object() || !doc.contains("preparer") || !doc["preparer"].is_object()) {
        throw DcDeterminismError("malformed or empty prepared.json: " + json_name);
    }
    for (const char* key : {"prepared_sha256", "record_digest"}) {
        if (!doc.contains(key) || !is_hex64(doc[key])) {
            throw DcDeterminismError(std::string("prepared.json ") + key + " is not a SHA-256 hex string");
        }
    }
    const Json& shots = doc["shots"];
    std::size_t count = shots.size();
    std::string bin_name = path_to_utf8(bin_path);
    std::string data = read_file(bin_path);
    std::size_t expected_size = kPreparedHeaderSize + kRecordSize * count;
    if (data.size() != expected_size) {
        throw DcDeterminismError(bin_name + " has size " + std::to_string(data.size()) + ", expected " +
                         std::to_string(expected_size) + " for " + std::to_string(count) + " shots");
    }
    if (data.compare(0, 8, kPreparedMagic) != 0 || read_u32(data, 8) != count) {
        throw DcDeterminismError(bin_name + ": bad magic or record count differs from prepared.json");
    }
    for (std::size_t position = 0; position < count; ++position) {
        std::uint32_t index = read_u32(data, kPreparedHeaderSize + position * kRecordSize);
        const Json& shot = shots[position];
        if (!shot.is_object() || !shot.contains("index") || shot["index"] != index) {
            throw DcDeterminismError(bin_name + ": record " + std::to_string(position) + " index " + std::to_string(index) +
                             " differs from prepared.json");
        }
    }
    if (sha256_hex(data) != doc["prepared_sha256"].get<std::string>()) {
        throw DcDeterminismError(bin_name + ": SHA-256 differs from prepared.json prepared_sha256");
    }
    if (sha256_hex(data.substr(kPreparedHeaderSize)) != doc["record_digest"].get<std::string>()) {
        throw DcDeterminismError(bin_name + ": record digest differs from prepared.json");
    }
    for (const char* key : {"input_sha256", "count"}) {
        if (!doc["corpus"].contains(key)) throw DcDeterminismError(std::string("prepared.json corpus lacks ") + key);
    }
    PreparedCorpus out;
    out.document = std::move(doc);
    out.bin_path = resolve(bin_path);
    return out;
}

std::int64_t recorded_limit(const Json& corpus) {
    if (corpus.contains("limit") && corpus["limit"].is_number_integer() && corpus["limit"].get<std::int64_t>() >= 0) {
        return corpus["limit"].get<std::int64_t>();
    }
    if (corpus.contains("count") && corpus.contains("skipped_concede") && corpus["count"].is_number_integer() &&
        corpus["skipped_concede"].is_number_integer()) {
        std::int64_t count = corpus["count"].get<std::int64_t>();
        std::int64_t skipped = corpus["skipped_concede"].get<std::int64_t>();
        if (count > 0 && skipped >= 0) return count + skipped;
    }
    throw DcDeterminismError("prepared.json records no discovery limit and none can be derived; re-run `prepare`");
}

std::unique_ptr<Catalogue> rebuild_catalogue(const fs::path& logs_dir, const PreparedCorpus& prepared) {
    std::error_code ec;
    if (!fs::is_directory(logs_dir, ec)) throw DcDeterminismError("logs directory not found: " + path_to_utf8(logs_dir));
    const Json& corpus = prepared.document["corpus"];
    log("cataloguing logs for verification ...");
    std::unique_ptr<Catalogue> catalogue(new Catalogue(resolve(logs_dir), recorded_limit(corpus)));
    if (!corpus["input_sha256"].is_string() || catalogue->input_sha256() != corpus["input_sha256"].get<std::string>()) {
        throw DcDeterminismError("--logs-dir does not match the prepared corpus (shot manifest digest differs)");
    }
    if (Json(static_cast<std::uint64_t>(catalogue->count())) != corpus["count"]) {
        throw DcDeterminismError("--logs-dir has " + std::to_string(catalogue->count()) + " shots, the prepared corpus " +
                         corpus["count"].dump());
    }
    const Json& shots = prepared.document["shots"];
    bool same = shots.size() == catalogue->count();
    for (std::size_t i = 0; same && i < shots.size(); ++i) {
        const ShotRow& row = catalogue->rows()[i];
        const Json& shot = shots[i];
        same = shot.is_object() && shot.contains("index") && shot["index"] == row.index &&
               shot.contains("shot_id") && shot["shot_id"] == row.shot_id && shot.contains("fgz") &&
               shot["fgz"] == row.fgz;
    }
    if (!same) throw DcDeterminismError("--logs-dir shot list differs from the prepared corpus");
    return catalogue;
}

Json prepare_corpus(const fs::path& logs_dir, std::int64_t limit, const fs::path& output, const Json& tool_identity,
                    std::unique_ptr<Catalogue>* keep_catalogue) {
    fs::path bin_final = output / kPreparedBinName;
    fs::path json_final = output / kPreparedJsonName;
    refuse_existing(bin_final);
    refuse_existing(json_final);
    std::error_code ec;
    if (!fs::is_directory(logs_dir, ec)) throw DcDeterminismError("logs directory not found: " + path_to_utf8(logs_dir));

    Json preparer = Json::object();
    preparer["dc_determinism"] = tool_identity;

    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    log("cataloguing logs ...");
    std::unique_ptr<Catalogue> catalogue(new Catalogue(resolve(logs_dir), limit));
    double catalogue_s = elapsed(t0);
    if (catalogue->count() == 0) throw DcDeterminismError("no shots found in " + path_to_utf8(logs_dir));
    std::size_t total = catalogue->count();
    log("catalogue: " + std::to_string(total) + " shots in " + seconds_text(catalogue_s) + " s");

    static JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "end"});
        tree.add({"log", "shot"});
        tree.add({"log", "actual_move"});
        tree.add({"log", "simulator_storage"});
        tree.add({"log", "trajectory", "frames"});
        return tree;
    }();
    ShotSource source(*catalogue);
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last = t1;
    std::string bin(kPreparedMagic, 8);
    append_u32(bin, static_cast<std::uint32_t>(total));
    bin.reserve(kPreparedHeaderSize + kRecordSize * total);
    Json shots = Json::array();
    std::size_t done = 0;
    for (const ShotRow& row : catalogue->rows()) {
        LoadedShot item = source.load(source.locate(row.index), &keep);
        CapiInitial initial = build_capi_initial(item.log, true);
        bin += encode_record(static_cast<std::uint32_t>(row.index), item.sheet_width, initial.coord);
        Json entry = Json::object();
        entry["index"] = row.index;
        entry["shot_id"] = row.shot_id;
        entry["fgz"] = row.fgz;
        shots.push_back(std::move(entry));
        ++done;
        if (elapsed(last) >= 5.0) {
            log("prepared " + std::to_string(done) + "/" + std::to_string(total));
            last = std::chrono::steady_clock::now();
        }
    }
    double prepare_s = elapsed(t1);
    source.close();

    Json corpus = Json::object();
    corpus["input_sha256"] = catalogue->input_sha256();
    corpus["input_digest"] = catalogue->input_digest();
    corpus["count"] = static_cast<std::int64_t>(total);
    corpus["skipped_concede"] = catalogue->skipped();
    corpus["limit"] = limit;
    Json timing = Json::object();
    timing["catalogue_s"] = catalogue_s;
    timing["prepare_s"] = prepare_s;
    Json document = Json::object();
    document["schema"] = kPreparedSchema;
    document["prepared_sha256"] = sha256_hex(bin);
    document["record_digest"] = sha256_hex(bin.substr(kPreparedHeaderSize));
    document["corpus"] = corpus;
    document["preparer"] = preparer;
    document["timing"] = timing;
    document["shots"] = shots;

    write_file_new(bin_final, bin);
    write_json_new(json_final, document);
    log("prepared " + std::to_string(total) + " shots in " + seconds_text(prepare_s) + " s -> " +
        path_to_utf8(output));
    if (keep_catalogue != nullptr) *keep_catalogue = std::move(catalogue);
    return document;
}

}  // namespace dc_determinism
