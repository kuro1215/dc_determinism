// `run simulator`: repeated evaluation of a prepared corpus with fresh worker
// processes, the worker line protocol, and the reduction of the repetitions to per-shot rows.
//
// The repetition runner is deliberately general (a RunObserver adds the worker `O` requests that
// log comparison needs, `positions`/`request`/`parse` adapt it to other worker requests) so that
// the other commands (log comparison, capture, lockstep comparison) reuse it.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"
#include "sim/provenance.hpp"

namespace dc_determinism {

extern const char* const kEvaluationSchema;  // "dc-determinism-evaluation-v1"

// One worker result line: `<position> <status> <steps> <trajectory digest> <final digest> <sim ns>`.
struct Outcome {
    std::string status;  // "stopped" / "truncated"
    std::int64_t steps = 0;
    std::string trajectory_digest;
    std::string final_digest;
    std::int64_t sim_ns = 0;
};

// `parse_line`: throws DcDeterminismError("worker reported: ..." / "unexpected worker output for position N: ...").
Outcome parse_line(const std::string& line, std::int64_t position);

// Sends every request through `request()` and hands each result line, plus the one extra line
// the worker sends after it, to `consume()` (the `O` requests of log comparison).
class RunObserver {
public:
    virtual ~RunObserver() = default;
    virtual std::string request(std::int64_t position) = 0;
    virtual void consume(std::int64_t position, const Outcome& outcome, const std::string& extra_line) = 0;
    // Called when every position is done; `remaining_seconds` < 0: no time limit.
    virtual void finish(double remaining_seconds) = 0;
    // Called on every exit path of the repetition.
    virtual void cancel() = 0;
};

struct RunHooks {
    RunObserver* observer = nullptr;
    // The worker positions to run (results keep this order); null: 0..total-1.
    const std::vector<std::int64_t>* positions = nullptr;
    // Formats a request when there is no observer (default: the position).
    std::function<std::string(std::int64_t)> request;
    // Replaces the plain result-line parser.
    std::function<Outcome(const std::string&, std::int64_t)> parse;
};

// One repetition: `workers` fresh processes, positions fed dynamically. Returns the wall time in
// seconds and the outcomes in position order. On any error, EOF or timeout every worker is
// killed first and only then are the feeder threads joined, so a worker blocked on a shot can
// never stall the failure path. `timeout_seconds` <= 0: no limit.
std::pair<double, std::vector<Outcome>> run_repetition(const std::vector<std::string>& command,
                                                       std::size_t total, int workers, double timeout_seconds,
                                                       const std::function<void(std::size_t)>& progress,
                                                       const RunHooks& hooks = RunHooks());

// `reduce_shots`: `shots` are the prepared.json shot entries; `runs[r][position]` the outcomes.
Json reduce_shots(const Json& shots, const std::vector<std::vector<Outcome>>& runs);

// `make_progress`: logs `repetition i/n; shots done/total` at most once a second (and at the end).
std::function<void(std::size_t)> make_progress(int index, std::int64_t repeats, std::size_t total);

// `resolve_worker`: the command prefix and the `worker` provenance block. `worker_command` is the
// `--worker-command` JSON array (test hook) or empty.
std::pair<std::vector<std::string>, Json> resolve_worker(const std::string& worker_command);

// What a command that starts simulator workers needs: the verified loader, the command prefix and
// the `worker` provenance block.
struct SimulatorTarget {
    ResolvedLoader loader;
    std::vector<std::string> prefix;
    Json worker;
};

// `loader_provenance` followed by `resolve_worker` for msvc/gcc/clang. `--compiler wasm` is the
// prebuilt WebAssembly bundle (sim/wasm.hpp): the prefix is `[node, dc_worker.js]` and the loader the
// `.wasm` file; with a `--worker-command` hook the hook replaces the prefix and the worker block only.
SimulatorTarget resolve_simulator(const std::string& compiler, const std::string& worker_command);

}  // namespace dc_determinism
