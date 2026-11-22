#include "sim/compare.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/provenance.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {

const char* const kComparisonSchema = "dc-determinism-comparison-v2";

namespace {

const char* const kObservation = "raw-stonecoordinate-v1";
const char* const kContractKeys[] = {"engine", "observation", "max_steps", "record_digest", "corpus_input_sha256"};

struct Mode {
    const char* name;
    const char* digest_key;
    const char* flag_key;
};
const Mode kModes[] = {{"trajectory", "trajectory_digest", "reproducible_trajectory"},
                       {"final", "final_digest", "reproducible_final"}};

void need(bool condition, const std::string& path, const std::string& message) {
    if (!condition) throw DcDeterminismError("invalid result " + path + ": " + message);
}

bool is_int(const Json& value) { return value.is_number_integer(); }

bool is_hash(const Json& value) { return value.is_string() && is_hex64(value.get<std::string>()); }

Json summary(const std::string& sha256, const Json& data) {
    Json loader = Json::object();
    for (const char* key : {"sha256", "compiler", "platform", "origin"}) {
        loader[key] = json_get(data["loader"], key);
    }
    Json worker = Json::object();
    for (const char* key : {"sha256", "version"}) worker[key] = json_get(data["worker"], key);
    Json out = Json::object();
    out["sha256"] = sha256;
    out["loader"] = loader;
    out["worker"] = worker;
    out["environment"] = data["environment"];
    out["repeats"] = data["repeats"];
    out["workers"] = json_get(data, "workers");
    return out;
}

}  // namespace

Json load_result(const std::string& path) {
    Json data = read_json(path_from_utf8(path));
    need(data.is_object() && json_get(data, "schema") == kEvaluationSchema, path,
         std::string("schema is not ") + kEvaluationSchema);
    Json contract = json_get(data, "contract");
    need(contract.is_object(), path, "missing contract");
    for (const char* key : kContractKeys) need(contract.contains(key), path, std::string("contract lacks ") + key);
    need(contract.contains("loader_sha256"), path, "contract lacks loader_sha256");
    need(contract["observation"] == Json(kObservation), path, "unknown observation");
    need(contract["engine"] == Json("fcv1") || contract["engine"] == Json("fcv1plus"), path, "unknown engine");
    need(is_int(contract["max_steps"]) && contract["max_steps"] > Json(0), path, "invalid max_steps");
    for (const char* key : {"record_digest", "corpus_input_sha256", "loader_sha256"}) {
        need(is_hash(contract[key]), path, std::string("contract ") + key + " is not a SHA-256");
    }
    Json repeats = json_get(data, "repeats");
    need(is_int(repeats) && repeats >= Json(2), path, "needs at least 2 repetitions");
    for (const char* key : {"loader", "worker", "environment"}) {
        need(json_get(data, key).is_object(), path, std::string("missing ") + key);
    }
    Json shots = json_get(data, "shots");
    need(shots.is_array() && !shots.empty(), path, "no shots");
    std::set<std::string> seen;
    for (const Json& row : shots) {
        need(row.is_object() && json_get(row, "shot_id").is_string(), path, "shot without shot_id");
        std::string id = row["shot_id"].get<std::string>();
        need(seen.insert(id).second, path, "duplicate shot_id " + id);
        Json status = json_get(row, "status");
        need(status == Json("stopped") || status == Json("truncated"), path,
             "shot " + id + " has an invalid status");
        Json steps = json_get(row, "steps");
        need(is_int(steps) && steps >= Json(0), path, "shot " + id + " has an invalid steps");
        for (const Mode& mode : kModes) {
            need(is_hash(json_get(row, mode.digest_key)), path,
                 "shot " + id + " has an invalid " + mode.digest_key);
            need(json_get(row, mode.flag_key).is_boolean(), path, "shot " + id + " has an invalid " + mode.flag_key);
        }
    }
    return data;
}

Json compare_results(const Json& a, const Json& b, const std::string& sha256_a, const std::string& sha256_b) {
    for (const char* key : kContractKeys) {
        if (a["contract"][key] != b["contract"][key]) {
            throw DcDeterminismError(std::string("incompatible results: contract ") + key + " differs (" +
                             py_repr(a["contract"][key]) + " vs " + py_repr(b["contract"][key]) + ")");
        }
    }
    Json identity = check_source_identity(a["loader"], b["loader"]);
    std::vector<std::string> ids_a, ids_b;
    for (const Json& row : a["shots"]) ids_a.push_back(row["shot_id"].get<std::string>());
    for (const Json& row : b["shots"]) ids_b.push_back(row["shot_id"].get<std::string>());
    if (ids_a != ids_b) throw DcDeterminismError("incompatible results: shot lists differ");

    Json report = Json::object();
    report["schema"] = kComparisonSchema;
    report["classification"] =
        a["contract"]["loader_sha256"] == b["contract"]["loader_sha256"] ? "same-binary" : "cross-binary";
    report["loader_identity"] = identity;
    Json contract = Json::object();
    for (const char* key : kContractKeys) contract[key] = a["contract"][key];
    report["contract"] = contract;
    report["total"] = ids_a.size();
    Json inputs = Json::array();
    inputs.push_back(summary(sha256_a, a));
    inputs.push_back(summary(sha256_b, b));
    report["inputs"] = inputs;

    std::set<std::string> excluded_ids;
    std::size_t mismatch_total = 0;
    for (const Mode& mode : kModes) {
        std::size_t compared = 0, excluded = 0;
        Json mismatches = Json::array();
        for (std::size_t i = 0; i < a["shots"].size(); ++i) {
            const Json& x = a["shots"][i];
            const Json& y = b["shots"][i];
            bool eligible = x["status"] == Json("stopped") && y["status"] == Json("stopped") &&
                            x[mode.flag_key].get<bool>() && y[mode.flag_key].get<bool>();
            if (!eligible) {
                ++excluded;
                excluded_ids.insert(x["shot_id"].get<std::string>());
                continue;
            }
            ++compared;
            if (x[mode.digest_key] != y[mode.digest_key]) mismatches.push_back(x["shot_id"]);
        }
        mismatch_total += mismatches.size();
        Json block = Json::object();
        block["compared"] = compared;
        block["excluded"] = excluded;
        block["mismatches"] = mismatches.size();
        block["mismatch_shots"] = mismatches;
        if (compared > 0) {
            block["mismatch_rate"] = static_cast<double>(mismatches.size()) / static_cast<double>(compared);
        } else {
            block["mismatch_rate"] = nullptr;
        }
        report[mode.name] = block;
    }
    report["excluded"] = excluded_ids.size();
    Json excluded_list = Json::array();
    for (const std::string& id : excluded_ids) excluded_list.push_back(id);  // sorted() of str
    report["excluded_shots"] = excluded_list;
    report["identical"] = mismatch_total == 0 && excluded_ids.empty() && report["total"] > Json(0);
    return report;
}

bool is_archive(const fs::path& path) {
    // zipfile._EndRecData: the last "PK\5\6" in the final 64 KiB + 22 bytes with a full 22-byte
    // end-of-central-directory record behind it.
    const std::uint64_t kEnd = 22;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < static_cast<std::streamoff>(kEnd)) return false;
    std::streamoff window = std::min<std::streamoff>(size, (1 << 16) + static_cast<std::streamoff>(kEnd));
    std::string tail(static_cast<std::size_t>(window), '\0');
    file.seekg(size - window, std::ios::beg);
    file.read(&tail[0], window);
    if (file.gcount() != window) return false;
    std::size_t at = tail.rfind(std::string("PK\x05\x06", 4));
    return at != std::string::npos && tail.size() - at >= kEnd;
}

}  // namespace dc_determinism