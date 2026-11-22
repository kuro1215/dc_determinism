#include "pybind/pybind.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <memory>
#include <thread>

#include "capture/dctr.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/tempdir.hpp"
#include "common/text.hpp"
#include "corpus/capi_initial.hpp"
#include "corpus/pytext.hpp"
#include "json/writer.hpp"
#include "process/process.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {

const char* const kPybindRequestSchema = "dc-pybind-request-v1";
const char* const kPybindResultSchema = "dc-pybind-result-v1";

namespace {

constexpr double kProgressIntervalSeconds = 5.0;

using Clock = std::chrono::steady_clock;

// The last `count` bytes of `text` (not starting inside a UTF-8 sequence).
std::string tail_text(const std::string& text, std::size_t count) {
    if (text.size() <= count) return text;
    std::size_t start = text.size() - count;
    while (start < text.size() && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) ++start;
    return text.substr(start);
}

const char* kind_label(const std::string& kind) { return kind == "record" ? "recording worker" : "worker"; }

fs::path worker_script() { return repo_root() / "src" / "worker" / "pybind" / "pybind_worker.py"; }

}  // namespace

const std::vector<std::string>& pybind_engines() {
    static const std::vector<std::string> engines = {"py27", "py36"};
    return engines;
}

std::string pybind_name(const std::string& engine) { return "fcv1-" + engine; }

std::string pybind_platform_refusal(const std::string& platform) {
    if (platform == "linux-amd64") return std::string();
    if (platform == "windows-amd64") {
        return "requires Linux (run it in the dc_determinism-cpu:local image: dc_determinism.ps1 and dc_determinism.sh do)";
    }
    return "requires an x86-64 Linux binary (the official pybind extensions are x86-64 only; run it in an amd64 "
           "dc_determinism-cpu:local image)";
}

void require_pybind_platform(const std::string& what) {
    std::string reason = pybind_platform_refusal(platform_key());
    if (!reason.empty()) throw DcDeterminismError(what + " " + reason);
}

// --- binaries ----------------------------------------------------------------------------------

fs::path pybind_dir() { return repo_root() / "prebuilt" / "pybind"; }

Json pybind_manifest() {
    fs::path path = pybind_dir() / "manifest.json";
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw DcDeterminismError("pybind manifest not found: " + path_text(path));
    Json manifest = read_json(path);
    if (!manifest.is_object() || !manifest.contains("engines") || !manifest["engines"].is_object()) {
        throw DcDeterminismError("invalid pybind manifest: " + path_text(path));
    }
    return manifest;
}

Json manifest_entry(const std::string& engine, const std::string& kind) {
    Json manifest = pybind_manifest();
    const Json& engines = manifest["engines"];
    std::string name = pybind_name(engine);
    if (engines.contains(name) && engines[name].is_object() && engines[name].contains(kind)) {
        const Json& entry = engines[name][kind];
        if (entry.is_object() && entry.contains("sha256") && entry["sha256"].is_string()) return entry;
    }
    throw DcDeterminismError("the pybind manifest has no " + kind + " entry for " + name);
}

PybindBinary resolve_pybind_binary(const std::string& engine, const std::string& kind,
                                   const std::string& override_path) {
    Json entry = manifest_entry(engine, kind);
    std::string suffix = kind == "record" ? "record-v2" : "official";
    fs::path path = override_path.empty()
                        ? pybind_dir() / pybind_name(engine) / ("simulator-" + suffix + ".so")
                        : resolve(path_from_utf8(override_path));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw DcDeterminismError(kind + " binary not found: " + path_text(path));
    std::string digest = sha256_file(path);
    std::string expected = entry["sha256"].get<std::string>();
    if (digest != expected) {
        throw DcDeterminismError(path_text(path) + " is not the " + kind + " " + pybind_name(engine) +
                         " binary of prebuilt/pybind/manifest.json (SHA-256 " + digest + ", expected " + expected +
                         ")");
    }
    return {path, digest};
}

// --- the worker --------------------------------------------------------------------------------

PybindWorker::PybindWorker(const std::string& hook_text) {
    info_ = Json::object();
    if (!hook_text.empty()) {
        Json prefix = Json::parse(hook_text, nullptr, false);
        if (prefix.is_discarded()) throw DcDeterminismError("--pybind-worker-command must be a JSON array: invalid JSON");
        bool ok = prefix.is_array() && !prefix.empty();
        if (ok) {
            for (const Json& item : prefix) ok = ok && item.is_string();
        }
        if (!ok) throw DcDeterminismError("--pybind-worker-command must be a non-empty JSON array of strings");
        prefix_ = prefix.get<std::vector<std::string>>();
        info_["path"] = nullptr;
        info_["sha256"] = nullptr;
        info_["command"] = prefix;
        return;
    }
    fs::path script = worker_script();
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) throw DcDeterminismError("the pybind worker was not found: " + path_text(script));
    std::optional<fs::path> python = find_on_path("python3");
    if (!python) throw DcDeterminismError("python3 was not found on PATH (the pybind features run in the dc_determinism-cpu:local image)");
    prefix_ = {path_text(*python), "-I", path_text(script)};
    info_["path"] = path_text(script);
    info_["sha256"] = sha256_file(script);
}

std::vector<std::string> PybindWorker::command(const PybindBinary& binary, const std::string& engine,
                                               const std::string& kind) const {
    std::vector<std::string> out = prefix_;
    for (const std::string& part : {std::string("--binary"), path_text(binary.path), std::string("--sha256"),
                                    binary.sha256, std::string("--engine"), pybind_name(engine), std::string("--kind"),
                                    kind}) {
        out.push_back(part);
    }
    return out;
}

Json PybindWorker::python_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return python_version_;
}

Json PybindWorker::numpy_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return numpy_version_;
}

void PybindWorker::note_versions(const Json& response) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (python_version_.is_null() && response.contains("python_version") && response["python_version"].is_string()) {
        python_version_ = response["python_version"];
    }
    if (numpy_version_.is_null() && response.contains("numpy_version") && response["numpy_version"].is_string()) {
        numpy_version_ = response["numpy_version"];
    }
}

Json PybindWorker::call(const PybindBinary& binary, const std::string& engine, const std::string& kind,
                        const Json& request, double timeout_seconds) const {
    std::string actual = sha256_file(binary.path);
    if (actual != binary.sha256) {
        throw DcDeterminismError(path_text(binary.path) + ": binary SHA-256 mismatch: " + actual);
    }
    StartOptions options;
    options.argv = command(binary, engine, kind);
    options.pipe_stdin = true;
    options.pipe_stdout = true;
    options.error_stream = ErrorStream::Pipe;
    std::string input;
    try {
        input = json_dumps(request, DumpOptions::compact(false, false));
    } catch (const DcDeterminismError& exc) {  // a non-finite value: only these shots fail
        throw WorkerFailure{std::string(kind == "record" ? "invalid recording worker output: " : "invalid worker response: ") +
                            exc.what()};
    }
    RunResult result;
    try {
        result = run_capture(options, input, timeout_seconds);
    } catch (const DcDeterminismError& exc) {
        throw WorkerFailure{std::string("cannot start the ") + kind_label(kind) + ": " + exc.what()};
    }
    if (result.timed_out) throw WorkerFailure{"timeout"};
    if (result.exit_code != 0) {
        std::string message = "pybind worker exited " + std::to_string(result.exit_code) + ": " +
                              tail_text(result.err, 4000);
        if (kind == "record") throw WorkerFailure{"recording worker failed: " + tail_text(message, 400)};
        throw WorkerFailure{"worker error: " + tail_text(message, 300)};
    }
    if (sha256_file(binary.path) != binary.sha256) {
        throw DcDeterminismError(path_text(binary.path) + ": binary changed during worker execution");
    }
    Json response;
    try {
        response = parse_json_text(result.out, "the pybind worker output");
    } catch (const DcDeterminismError& exc) {
        throw WorkerFailure{std::string(kind == "record" ? "invalid recording worker output: " : "invalid worker response: ") +
                            exc.what()};
    }
    if (!response.is_object() || !response.contains("schema") || response["schema"] != kPybindResultSchema ||
        !response.contains("rows") || !response["rows"].is_array()) {
        throw WorkerFailure{std::string("incomplete or out-of-order ") + (kind == "record" ? "recording " : "") +
                            "worker response"};
    }
    note_versions(response);
    return response;
}

// --- inputs ------------------------------------------------------------------------------------

Json normalize_shot(const LoadedShot& shot) {
    LegacyPybindInput input;
    try {
        input = legacy_pybind_input(shot.log);
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError(std::string("ValueError: ") + exc.what());
    }
    Json task = Json::object();
    task["shot_id"] = shot.shot_id;
    Json positions = Json::array();
    for (double value : input.positions) positions.push_back(value);
    task["initial_positions"] = positions;
    task["total_shot_number"] = input.total_shot_number;
    task["shot_per_team"] = input.shot_per_team;
    task["velocity_x"] = input.velocity_x;
    task["velocity_y"] = input.velocity_y;
    task["angular_velocity"] = input.angular_velocity;
    task["team_number"] = input.team_number;
    task["applied_rule"] = 0;
    return task;
}

Json adapt_task(const Json& task, const std::string& engine) {
    Json out = task;
    if (engine == "py36") out["angular_velocity"] = -task["angular_velocity"].get<double>();
    return out;
}

Json input_normalization() {
    Json out = Json::object();
    out["profile"] = "legacy";
    out["function"] = "the legacy pybind input of the old logs (dc_determinism pybind/pybind.cpp)";
    out["removal_sheet_width"] = "the sheet width of each prepared shot";
    out["frame_stride"] = 1;
    return out;
}

std::vector<double> read_widths(const PreparedCorpus& prepared, std::size_t count) {
    std::vector<PreparedRecord> records = decode_prepared(read_file(prepared.bin_path));
    if (records.size() < count) throw DcDeterminismError("prepared.bin holds fewer shots than the prepared corpus");
    std::vector<double> widths;
    widths.reserve(count);
    for (std::size_t i = 0; i < count; ++i) widths.push_back(static_cast<double>(records[i].width));
    return widths;
}

std::vector<std::string> read_shot_ids(const std::string& path) {
    std::string text;
    try {
        text = read_file(path_from_utf8(path));
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("cannot read shot list " + path + ": " + exc.what());
    }
    if (!is_valid_utf8(text)) throw DcDeterminismError("cannot read shot list " + path + ": invalid UTF-8");
    std::vector<std::string> ids;
    for (const std::string& line : py_splitlines(text)) {
        std::string stripped = py_strip(line);
        if (!stripped.empty()) ids.push_back(stripped);
    }
    return ids;
}

// --- frames ------------------------------------------------------------------------------------

std::vector<float> read_frames(const fs::path& path, const Json& frame_count) {
    if (!frame_count.is_number_integer() || frame_count.get<std::int64_t>() < 1) {
        throw DcDeterminismError("invalid frame_count " + py_repr(frame_count));
    }
    std::int64_t count = frame_count.get<std::int64_t>();
    std::string data = read_file(path);
    std::size_t expected = static_cast<std::size_t>(count) * kFrameBytes;
    if (data.size() != expected) {
        throw DcDeterminismError(path_to_utf8(path.filename()) + " has " + std::to_string(data.size()) + " bytes, expected " +
                         std::to_string(expected) + " (frame_count " + std::to_string(count) + ")");
    }
    std::vector<float> frames(static_cast<std::size_t>(count) * kFrameSlots * kFrameFields);
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data()) + 4 * i;
        std::uint32_t bits = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                             (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        std::memcpy(&frames[i], &bits, sizeof(bits));
    }
    return frames;
}

std::string frames_to_payload(const std::vector<float>& frames, std::int64_t frame_count, const std::string& shot_id,
                              std::int64_t index, const std::string& engine, std::int64_t* stored_frames) {
    std::size_t count = static_cast<std::size_t>(frame_count);
    if (frames.size() != count * kFrameSlots * kFrameFields) {
        throw DcDeterminismError("invalid recording frame layout: " + std::to_string(frames.size()) + " values for " +
                         std::to_string(frame_count) + " frames");
    }
    bool present[kFrameSlots] = {};
    for (std::size_t frame = 0; frame < count; ++frame) {
        for (std::size_t slot = 0; slot < kFrameSlots; ++slot) {
            for (std::size_t field = 0; field < kFrameFields; ++field) {
                float value = frames[(frame * kFrameSlots + slot) * kFrameFields + field];
                if (!(value == 0.0f)) present[slot] = true;  // NaN != 0 as in numpy
            }
        }
    }
    std::uint32_t mask = 0;
    std::vector<std::size_t> stones;
    for (std::size_t slot = 0; slot < kFrameSlots; ++slot) {
        if (present[slot]) {
            mask |= std::uint32_t{1} << slot;
            stones.push_back(slot);
        }
    }
    std::string raw;
    raw.reserve(8 + count * stones.size() * kFrameFields * 4);
    auto put32 = [&raw](std::uint32_t value) {
        for (int b = 0; b < 4; ++b) raw.push_back(static_cast<char>((value >> (8 * b)) & 0xFF));
    };
    put32(mask);
    put32(static_cast<std::uint32_t>(count));
    for (std::size_t frame = 0; frame < count; ++frame) {
        for (std::size_t slot : stones) {
            for (std::size_t field = 0; field < kFrameFields; ++field) {
                std::uint32_t bits;
                float value = frames[(frame * kFrameSlots + slot) * kFrameFields + field];
                std::memcpy(&bits, &value, sizeof(bits));
                put32(bits);
            }
        }
    }
    return raw_to_payload(raw, shot_id, index, engine, stored_frames);
}

// --- batches -----------------------------------------------------------------------------------

namespace {

// Runs `batch_count` jobs on `workers` threads; `job(i)` may throw, the first exception in index order is
// rethrown once every started job has finished. After the first failure no new job is started.
void run_batches(std::size_t batch_count, int workers, const std::function<void(std::size_t)>& job) {
    std::vector<std::exception_ptr> errors(batch_count);
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    auto body = [&] {
        for (;;) {
            if (failed.load()) return;
            std::size_t i = next.fetch_add(1);
            if (i >= batch_count) return;
            try {
                job(i);
            } catch (...) {
                errors[i] = std::current_exception();
                failed.store(true);
            }
        }
    };
    if (workers <= 1) {
        body();
    } else {
        std::vector<std::thread> threads;
        for (int t = 0; t < workers; ++t) threads.emplace_back(body);
        for (std::thread& thread : threads) thread.join();
    }
    for (const std::exception_ptr& error : errors) {
        if (error) std::rethrow_exception(error);
    }
}

std::vector<Json> call_official(const PybindWorker& worker, const std::string& engine, const PybindBinary& binary,
                                const std::vector<Json>& shots, double timeout_seconds) {
    Json request = Json::object();
    request["schema"] = kPybindRequestSchema;
    Json list = Json::array();
    for (const Json& shot : shots) list.push_back(shot);
    request["shots"] = list;
    request["frame_stride"] = 10;
    request["removal_sheet_width"] = 0.0;
    Json response = worker.call(binary, engine, "official", request, timeout_seconds);
    const Json& rows = response["rows"];
    bool ok = rows.size() == shots.size();
    for (std::size_t i = 0; ok && i < rows.size(); ++i) {
        ok = rows[i].is_object() && rows[i].contains("shot_id") && rows[i]["shot_id"] == shots[i]["shot_id"];
    }
    if (!ok) throw WorkerFailure{"incomplete or out-of-order worker response"};
    std::vector<Json> out;
    for (const Json& row : rows) {
        Json entry = Json::object();
        entry["shot_id"] = row["shot_id"];
        entry["final"] = row.contains("final") ? row["final"] : Json(nullptr);
        out.push_back(std::move(entry));
    }
    return out;
}

}  // namespace

std::map<std::int64_t, Json> run_official(const PybindWorker& worker, const std::string& engine,
                                          const PybindBinary& binary,
                                          const std::vector<std::pair<std::int64_t, Json>>& tasks,
                                          double timeout_seconds, int workers) {
    std::map<std::int64_t, Json> results;
    std::mutex mutex;
    std::size_t done = 0;
    Clock::time_point last = Clock::now();
    std::size_t batch_count = (tasks.size() + kOfficialBatch - 1) / kOfficialBatch;

    auto job = [&](std::size_t number) {
        std::size_t begin = number * kOfficialBatch;
        std::size_t end = std::min(tasks.size(), begin + kOfficialBatch);
        std::vector<Json> shots;
        for (std::size_t i = begin; i < end; ++i) shots.push_back(tasks[i].second);
        std::map<std::int64_t, Json> outcome;
        try {
            std::vector<Json> rows = call_official(worker, engine, binary, shots, timeout_seconds);
            for (std::size_t i = begin; i < end; ++i) outcome[tasks[i].first] = rows[i - begin];
        } catch (const WorkerFailure&) {
            // every failed batch, a single-shot one included, gets one per-shot retry
            for (std::size_t i = begin; i < end; ++i) {
                try {
                    outcome[tasks[i].first] =
                        call_official(worker, engine, binary, std::vector<Json>(1, tasks[i].second), timeout_seconds)[0];
                } catch (const WorkerFailure& single) {
                    Json entry = Json::object();
                    entry["inconclusive"] = single.reason;
                    outcome[tasks[i].first] = entry;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& item : outcome) results[item.first] = std::move(item.second);
        done += end - begin;
        if (std::chrono::duration<double>(Clock::now() - last).count() >= kProgressIntervalSeconds ||
            done == tasks.size()) {
            last = Clock::now();
            log(engine + ": shots " + std::to_string(done) + "/" + std::to_string(tasks.size()));
        }
    };
    run_batches(batch_count, workers, job);
    return results;
}

void record_trajectories(const PybindWorker& worker, const std::string& engine, const PybindBinary& binary,
                         const std::vector<std::pair<std::int64_t, Json>>& items, double timeout_seconds, int workers,
                         const RecordConsumer& consume, const ProgressFn& progress) {
    std::mutex mutex;
    std::size_t done = 0;
    std::size_t batch_count = (items.size() + kRecordBatch - 1) / kRecordBatch;

    // One call on `batch` (items[begin, end)); the shots are consumed only after the whole call succeeded.
    auto call = [&](std::size_t begin, std::size_t end) {
        TempDir frames_dir("dc_determinism-frames-");
        Json request = Json::object();
        request["schema"] = kPybindRequestSchema;
        Json list = Json::array();
        for (std::size_t i = begin; i < end; ++i) list.push_back(items[i].second);
        request["shots"] = list;
        request["frame_stride"] = 1;
        request["removal_sheet_width"] = 0;
        request["frames_dir"] = path_text(frames_dir.path());
        Json response = worker.call(binary, engine, "record", request, timeout_seconds);
        const Json& rows = response["rows"];
        bool ok = rows.size() == end - begin;
        for (std::size_t i = begin; ok && i < end; ++i) {
            const Json& row = rows[i - begin];
            ok = row.is_object() && row.contains("shot_id") && row["shot_id"] == items[i].second["shot_id"];
        }
        if (!ok) throw WorkerFailure{"incomplete or out-of-order recording worker response"};
        std::vector<std::vector<float>> loaded;
        for (std::size_t i = begin; i < end; ++i) {
            const Json& row = rows[i - begin];
            try {
                if (!row.contains("frames_file") || !row["frames_file"].is_string()) {
                    throw DcDeterminismError("the row has no frames_file");
                }
                std::string name = row["frames_file"].get<std::string>();
                if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
                    name == "." || name == "..") {
                    throw DcDeterminismError("invalid frames_file " + py_repr(row["frames_file"]));
                }
                loaded.push_back(read_frames(frames_dir.path() / path_from_utf8(name),
                                             row.contains("frame_count") ? row["frame_count"] : Json(nullptr)));
            } catch (const DcDeterminismError& exc) {
                throw WorkerFailure{"cannot read the frames of " + items[i].second["shot_id"].get<std::string>() +
                                    ": " + exc.what()};
            }
        }
        for (std::size_t i = begin; i < end; ++i) consume(items[i].first, rows[i - begin], std::move(loaded[i - begin]));
    };

    auto job = [&](std::size_t number) {
        std::size_t begin = number * kRecordBatch;
        std::size_t end = std::min(items.size(), begin + kRecordBatch);
        try {
            call(begin, end);
        } catch (const WorkerFailure&) {
            for (std::size_t i = begin; i < end; ++i) {
                try {
                    call(i, i + 1);
                } catch (const WorkerFailure& failure) {
                    throw DcDeterminismError(pybind_name(engine) + " recording of shot " +
                                     items[i].second["shot_id"].get<std::string>() + " failed: " + failure.reason);
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        done += end - begin;
        if (progress) progress(done);
    };
    run_batches(batch_count, workers, job);
}

std::map<std::int64_t, Json> run_rows(const PybindWorker& worker, const std::string& engine,
                                      const PybindBinary& binary, const std::string& kind,
                                      const std::vector<std::pair<std::int64_t, Json>>& tasks,
                                      std::int64_t frame_stride, double timeout_seconds, int workers,
                                      const std::string& label) {
    std::map<std::int64_t, Json> results;
    std::mutex mutex;
    std::size_t done = 0;
    std::size_t batch_count = (tasks.size() + kRecordBatch - 1) / kRecordBatch;

    auto call = [&](std::size_t begin, std::size_t end) {
        Json request = Json::object();
        request["schema"] = kPybindRequestSchema;
        Json list = Json::array();
        for (std::size_t i = begin; i < end; ++i) list.push_back(tasks[i].second);
        request["shots"] = list;
        request["frame_stride"] = frame_stride;
        request["removal_sheet_width"] = 0;
        Json response = worker.call(binary, engine, kind, request, timeout_seconds);
        const Json& rows = response["rows"];
        bool ok = rows.size() == end - begin;
        for (std::size_t i = begin; ok && i < end; ++i) {
            ok = rows[i - begin].is_object() && rows[i - begin].contains("shot_id") &&
                 rows[i - begin]["shot_id"] == tasks[i].second["shot_id"];
        }
        if (!ok) throw WorkerFailure{"incomplete or out-of-order worker response"};
        return rows;
    };

    auto job = [&](std::size_t number) {
        std::size_t begin = number * kRecordBatch;
        std::size_t end = std::min(tasks.size(), begin + kRecordBatch);
        std::map<std::int64_t, Json> outcome;
        try {
            Json rows = call(begin, end);
            for (std::size_t i = begin; i < end; ++i) outcome[tasks[i].first] = rows[i - begin];
        } catch (const WorkerFailure&) {
            for (std::size_t i = begin; i < end; ++i) {
                try {
                    outcome[tasks[i].first] = call(i, i + 1)[0];
                } catch (const WorkerFailure& single) {
                    Json entry = Json::object();
                    entry["error"] = single.reason;
                    outcome[tasks[i].first] = entry;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& item : outcome) results[item.first] = std::move(item.second);
        done += outcome.size();
        log(label + ": shots " + std::to_string(done) + "/" + std::to_string(tasks.size()));
    };
    run_batches(batch_count, workers, job);
    return results;
}

std::uint64_t double_bits(double value) {
    std::uint64_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool bitwise_final_equal(const Json& record_final, const Json& official_final) {
    if (!record_final.is_array() || !official_final.is_array() || record_final.size() != official_final.size()) {
        return false;
    }
    for (std::size_t team = 0; team < record_final.size(); ++team) {
        const Json& left_team = record_final[team];
        const Json& right_team = official_final[team];
        if (!left_team.is_array() || !right_team.is_array() || left_team.size() != right_team.size()) return false;
        for (std::size_t stone = 0; stone < left_team.size(); ++stone) {
            const Json& left = left_team[stone];
            const Json& right = right_team[stone];
            if (!left.is_array() || !right.is_array() || left.size() != right.size()) return false;
            for (std::size_t axis = 0; axis < left.size(); ++axis) {
                if (!left[axis].is_number() || !right[axis].is_number()) return false;
                if (double_bits(left[axis].get<double>()) != double_bits(right[axis].get<double>())) return false;
            }
        }
    }
    return true;
}

}  // namespace dc_determinism
