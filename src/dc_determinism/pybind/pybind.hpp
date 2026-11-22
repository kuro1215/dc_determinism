// The historical pybind engines (fcv1-py27, fcv1-py36), Linux amd64 only: binaries, inputs and the isolated
// worker `src/worker/pybind/pybind_worker.py` (python3 inside the dc_determinism-cpu:local image), shared by
// `compare pybind`, `compare pybind --check-recording` and `capture simulator --engine pybind-*`.
//
// The worker is one process per call: JSON request on stdin (`dc-pybind-request-v1`), JSON response on stdout
// (`dc-pybind-result-v1`, with the python and numpy versions it ran with). Kind `official` returns the final
// board of a shot; kind `record` additionally returns the step count, the removed steps, the frame count, the
// last frame and, when the request names `frames_dir`, writes the frames of every shot there as `<n>.f32` (raw
// little-endian float32, shape [frame_count, 16, 6], C order; x, y, angle, vx, vy, w per pybind stone slot
// `team * 8 + index`, six zeros for an absent stone).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/platform.hpp"
#include "corpus/catalogue.hpp"
#include "corpus/prepared.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kPybindRequestSchema;  // "dc-pybind-request-v1"
extern const char* const kPybindResultSchema;   // "dc-pybind-result-v1"
constexpr std::size_t kOfficialBatch = 50;
constexpr std::size_t kRecordBatch = 25;
constexpr std::size_t kFrameSlots = 16;
constexpr std::size_t kFrameFields = 6;
constexpr std::size_t kFrameBytes = kFrameSlots * kFrameFields * 4;

// "py27", "py36".
const std::vector<std::string>& pybind_engines();
// "fcv1-py27" / "fcv1-py36".
std::string pybind_name(const std::string& engine);

// The empty string when the platform may run the pybind features (linux-amd64, the only one the official
// extensions exist for), else the reason they are refused. `platform` is a key of `platform_key()`.
std::string pybind_platform_refusal(const std::string& platform);
// DcDeterminismError("<what> requires Linux amd64 ...") on this platform when refused.
void require_pybind_platform(const std::string& what);

// --- binaries ----------------------------------------------------------------------------------

struct PybindBinary {
    fs::path path;
    std::string sha256;
};

fs::path pybind_dir();
// prebuilt/pybind/manifest.json; DcDeterminismError when missing or invalid.
Json pybind_manifest();
// The `official` or `record` entry of an engine in the manifest.
Json manifest_entry(const std::string& engine, const std::string& kind);
// Path and expected SHA-256 of a `record` (v2) or `official` binary; the file is verified against the manifest
// (an explicit path must be the same binary). `override_path` empty: prebuilt/pybind/<engine>/simulator-*.so.
PybindBinary resolve_pybind_binary(const std::string& engine, const std::string& kind,
                                   const std::string& override_path);

// --- the worker --------------------------------------------------------------------------------

// A failure of one worker call that may be specific to the shots of that call (timeout, nonzero exit,
// unreadable or incomplete answer, unreadable frames): the callers retry shot by shot.
struct WorkerFailure {
    std::string reason;
};

// The command of the worker and its provenance. `hook_text` is the hidden `--pybind-worker-command` JSON array
// (a test stand-in for `python3 -I pybind_worker.py`; empty: the real worker, which needs python3 on PATH).
class PybindWorker {
public:
    explicit PybindWorker(const std::string& hook_text);

    // The call arguments after the prefix: `--binary --sha256 --engine --kind`.
    std::vector<std::string> command(const PybindBinary& binary, const std::string& engine,
                                     const std::string& kind) const;
    // {"path", "sha256"} of src/worker/pybind/pybind_worker.py; with a hook {"path": null, "sha256": null,
    // "command": [...]}.
    const Json& info() const { return info_; }

    // The versions the worker reported (null before the first answer).
    Json python_version() const;
    Json numpy_version() const;
    void note_versions(const Json& response) const;

    // One call; `kind` is "official" or "record". Throws WorkerFailure (see above) or DcDeterminismError (a binary that
    // changed is fatal). The response has been checked for the schema only.
    Json call(const PybindBinary& binary, const std::string& engine, const std::string& kind, const Json& request,
              double timeout_seconds) const;

private:
    std::vector<std::string> prefix_;
    Json info_;
    mutable std::mutex mutex_;
    mutable Json python_version_ = nullptr;
    mutable Json numpy_version_ = nullptr;
};

// --- inputs ------------------------------------------------------------------------------------

// The pybind input of one shot (the legacy profile): {shot_id, initial_positions, total_shot_number,
// shot_per_team, velocity_x, velocity_y, angular_velocity, team_number, applied_rule}. Throws DcDeterminismError.
Json normalize_shot(const LoadedShot& shot);
// fcv1-py36 gets the angular velocity negated (the C API convention); fcv1-py27 the value as it is.
Json adapt_task(const Json& task, const std::string& engine);
// The `input_normalization` provenance block.
Json input_normalization();
// The sheet width of the first `count` prepared shots (float32 in the record head of prepared.bin).
std::vector<double> read_widths(const PreparedCorpus& prepared, std::size_t count);
// `[line.strip() for ...]` of a shot-id file; DcDeterminismError when unreadable.
std::vector<std::string> read_shot_ids(const std::string& path);

// --- frames ------------------------------------------------------------------------------------

// The `.f32` file of one shot: raw little-endian float32, frame_count * 16 * 6 values; the size must be exactly
// frame_count * 16 * 6 * 4 bytes. Throws DcDeterminismError.
std::vector<float> read_frames(const fs::path& path, const Json& frame_count);
// A (F, 16, 6) frame stream as a `dctr-v1` payload: the stones that are present (not all zero) in at least one
// frame are stored, absent ones are six zeros, like unset C API slots.
std::string frames_to_payload(const std::vector<float>& frames, std::int64_t frame_count, const std::string& shot_id,
                              std::int64_t index, const std::string& engine, std::int64_t* stored_frames = nullptr);

// --- batches -----------------------------------------------------------------------------------

using ProgressFn = std::function<void(std::size_t)>;

// The official binary on `tasks` (position, adapted task) in batches of <= 50 on `workers` threads; a failed
// batch is retried shot by shot once. position -> {"shot_id", "final"} or {"inconclusive": reason}.
std::map<std::int64_t, Json> run_official(const PybindWorker& worker, const std::string& engine,
                                          const PybindBinary& binary,
                                          const std::vector<std::pair<std::int64_t, Json>>& tasks,
                                          double timeout_seconds, int workers);

// The recording binary on `items` (position, adapted task with removal_sheet_width) in batches of 25. For every
// shot `consume(position, row, frames)` is called (from a worker thread) with the worker row and the float32
// frames; a batch is consumed only after it succeeded as a whole. A failed batch is retried shot by shot; the
// first shot that still fails ends the batch and the run throws a DcDeterminismError ("fcv1-py27 recording of shot
// <id> failed: <reason>") after every batch has finished.
using RecordConsumer = std::function<void(std::int64_t, const Json&, std::vector<float>&&)>;
void record_trajectories(const PybindWorker& worker, const std::string& engine, const PybindBinary& binary,
                         const std::vector<std::pair<std::int64_t, Json>>& items, double timeout_seconds, int workers,
                         const RecordConsumer& consume, const ProgressFn& progress = ProgressFn());

// `--check-recording`: one kind ("official" or "record") on `tasks` in batches of 25 with the given frame stride and
// no frame files; a failed batch is retried shot by shot once. position -> the worker row (as the worker
// answered), or {"error": reason} for a shot that still fails.
std::map<std::int64_t, Json> run_rows(const PybindWorker& worker, const std::string& engine,
                                      const PybindBinary& binary, const std::string& kind,
                                      const std::vector<std::pair<std::int64_t, Json>>& tasks,
                                      std::int64_t frame_stride, double timeout_seconds, int workers,
                                      const std::string& label);

// Exact helpers of the bitwise comparisons.
std::uint64_t double_bits(double value);
// The v2 recording binary's final board against the official one, bit for bit (-0.0 differs from 0.0); false
// for a different shape or a non-number.
bool bitwise_final_equal(const Json& record_final, const Json& official_final);

}  // namespace dc_determinism
