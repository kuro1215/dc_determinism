#include "sim/runner.hpp"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>

#include "common/error.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "process/process.hpp"
#include "sim/provenance.hpp"
#include "sim/wasm.hpp"

namespace dc_determinism {

const char* const kEvaluationSchema = "dc-determinism-evaluation-v1";

namespace {

constexpr double kProgressIntervalSeconds = 1.0;
constexpr auto kPoll = std::chrono::milliseconds(100);

using Clock = std::chrono::steady_clock;

[[noreturn]] void unexpected_output(std::int64_t position, const std::string& text) {
    throw DcDeterminismError("unexpected worker output for position " + std::to_string(position) + ": " +
                     py_repr(Json(text)));
}

// The pipe to the worker broke while a request was written.
struct WriteFailure {};

}  // namespace

Outcome parse_line(const std::string& line, std::int64_t position) {
    std::string text = py_strip(line);
    if (text.compare(0, 3, "ERR") == 0) throw DcDeterminismError("worker reported: " + text);
    std::vector<std::string> fields = py_split(text);
    if (fields.size() != 6) unexpected_output(position, text);
    std::int64_t index = 0;
    Outcome outcome;
    if (!py_parse_int(fields[0], index) || index != position || !py_parse_int(fields[2], outcome.steps) ||
        !py_parse_int(fields[5], outcome.sim_ns)) {
        unexpected_output(position, text);
    }
    if ((fields[1] != "stopped" && fields[1] != "truncated") || !is_hex64(fields[3]) || !is_hex64(fields[4])) {
        unexpected_output(position, text);
    }
    outcome.status = fields[1];
    outcome.trajectory_digest = fields[3];
    outcome.final_digest = fields[4];
    return outcome;
}

std::pair<double, std::vector<Outcome>> run_repetition(const std::vector<std::string>& command,
                                                       std::size_t total, int workers, double timeout_seconds,
                                                       const std::function<void(std::size_t)>& progress,
                                                       const RunHooks& hooks) {
    std::vector<Outcome> results(total);
    std::vector<char> filled(total, 0);
    auto parse_result = hooks.parse ? hooks.parse : std::function<Outcome(const std::string&, std::int64_t)>(parse_line);

    std::mutex mutex;
    std::condition_variable wake;
    std::size_t next = 0;
    std::size_t done = 0;
    int alive = 0;
    bool failed = false;
    std::string error;

    auto fail = [&](const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!failed) {
            failed = true;
            error = message;
        }
        wake.notify_all();
    };
    auto has_failed = [&] {
        std::lock_guard<std::mutex> lock(mutex);
        return failed;
    };

    // "<exit code>" of a worker whose output ended: the code when it has exited, else "None".
    auto exit_text = [](Process& proc) {
        if (proc.wait_for(0.2)) return std::to_string(proc.exit_code());
        return std::string("None");
    };

    auto read_line = [&](Process& proc, std::string& line, const std::string& context) {
        if (!proc.read_line(line)) {
            throw DcDeterminismError("worker ended unexpectedly while " + context + " (exit code " + exit_text(proc) + ")");
        }
    };

    auto feed = [&](Process* proc) {
        std::int64_t position = -1;
        try {
            while (!has_failed()) {
                std::size_t slot;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (next >= total) break;
                    slot = next++;
                }
                position = hooks.positions == nullptr ? static_cast<std::int64_t>(slot) : (*hooks.positions)[slot];
                std::string line;
                try {
                    std::string text = hooks.observer != nullptr ? hooks.observer->request(position)
                                       : hooks.request           ? hooks.request(position)
                                                                  : std::to_string(position);
                    if (!proc->write_stdin(text + "\n")) throw WriteFailure();
                } catch (const WriteFailure&) {
                    // The worker probably died at startup; its ERR line explains why.
                    std::string reason;
                    proc->read_line(reason);
                    reason = py_strip(reason);
                    fail("worker failed before position " + std::to_string(position) + ": " +
                         (reason.empty() ? std::string("no output") : reason) + " (exit code " + exit_text(*proc) +
                         ")");
                    return;
                }
                if (!proc->read_line(line)) {
                    fail("worker ended unexpectedly while processing position " + std::to_string(position) +
                         " (exit code " + exit_text(*proc) + ")");
                    return;
                }
                try {
                    results[slot] = parse_result(line, position);
                    if (hooks.observer != nullptr) {
                        std::string extra;
                        read_line(*proc, extra, "sending sampled boards for position " + std::to_string(position));
                        hooks.observer->consume(position, results[slot], extra);
                    }
                    filled[slot] = 1;
                } catch (const DcDeterminismError& exc) {
                    fail(exc.what());
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex);
                ++done;
            }
            proc->close_stdin();
        } catch (const std::exception& exc) {
            fail("worker I/O error at position " + std::to_string(position) + ": " + exc.what());
        }
    };

    std::vector<std::unique_ptr<Process>> procs;
    std::vector<std::thread> threads;
    auto t0 = Clock::now();
    bool has_deadline = timeout_seconds > 0;
    auto deadline = t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(timeout_seconds));
    auto remaining_seconds = [&]() -> double {
        if (!has_deadline) return -1.0;
        double left = std::chrono::duration<double>(deadline - Clock::now()).count();
        return left > 0.0 ? left : 0.0;
    };

    auto cleanup = [&] {
        if (hooks.observer != nullptr) hooks.observer->cancel();
        for (auto& proc : procs) {  // always kill before joining anything
            if (proc->running()) proc->kill();
        }
        for (auto& proc : procs) proc->wait_for(30.0);
        for (std::thread& thread : threads) {
            if (thread.joinable()) thread.join();
        }
        threads.clear();
        procs.clear();
    };

    try {
        StartOptions options;
        options.argv = command;
        options.pipe_stdin = true;
        options.pipe_stdout = true;
        try {
            for (int i = 0; i < workers; ++i) procs.push_back(Process::start(options));
        } catch (const DcDeterminismError& exc) {
            throw DcDeterminismError(std::string("cannot start worker: ") + exc.what());
        }
        alive = static_cast<int>(procs.size());
        for (auto& proc : procs) {
            Process* raw = proc.get();
            threads.emplace_back([&, raw] {
                feed(raw);
                std::lock_guard<std::mutex> lock(mutex);
                --alive;
                wake.notify_all();
            });
        }

        for (;;) {
            std::size_t snapshot;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (failed || alive == 0) break;
                wake.wait_for(lock, kPoll);
                snapshot = done;
            }
            if (progress) progress(snapshot);
            if (has_deadline && Clock::now() > deadline) {
                fail("repetition timed out after " + format_g(timeout_seconds) + " s (" + std::to_string(snapshot) +
                     "/" + std::to_string(total) + " shots done)");
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (failed) throw DcDeterminismError(error);
        }

        if (hooks.observer != nullptr) hooks.observer->finish(remaining_seconds());

        // All positions are done; the workers see EOF on stdin and must exit with 0.
        for (auto& proc : procs) {
            double remaining = remaining_seconds();
            if (!proc->wait_for(remaining)) {
                throw DcDeterminismError("worker did not exit before the " + format_g(timeout_seconds) + " s timeout");
            }
            if (proc->exit_code() != 0) {
                throw DcDeterminismError("worker exited with code " + std::to_string(proc->exit_code()));
            }
        }
        if (progress) progress(total);
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
    double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    for (std::size_t i = 0; i < total; ++i) {
        if (!filled[i]) throw DcDeterminismError("internal error: missing results");
    }
    return {wall, std::move(results)};
}

Json reduce_shots(const Json& shots, const std::vector<std::vector<Outcome>>& runs) {
    Json rows = Json::array();
    for (std::size_t position = 0; position < shots.size(); ++position) {
        const Json& meta = shots[position];
        std::set<std::tuple<std::string, std::int64_t, std::string>> trajectories;
        std::set<std::pair<std::string, std::string>> finals;
        std::set<std::string> distinct_trajectory, distinct_final;
        bool all_stopped = true;
        for (const auto& run : runs) {
            const Outcome& rep = run[position];
            trajectories.emplace(rep.status, rep.steps, rep.trajectory_digest);
            finals.emplace(rep.status, rep.final_digest);
            distinct_trajectory.insert(rep.trajectory_digest);
            distinct_final.insert(rep.final_digest);
            all_stopped = all_stopped && rep.status == "stopped";
        }
        const Outcome& first = runs[0][position];
        Json row = Json::object();
        row["index"] = meta["index"];
        row["shot_id"] = meta["shot_id"];
        row["fgz"] = meta["fgz"];
        row["status"] = first.status;
        row["steps"] = first.steps;
        row["trajectory_digest"] = first.trajectory_digest;
        row["final_digest"] = first.final_digest;
        row["reproducible_trajectory"] = trajectories.size() == 1;
        row["reproducible_final"] = finals.size() == 1;
        row["distinct_trajectory_digests"] = distinct_trajectory.size();
        row["distinct_final_digests"] = distinct_final.size();
        row["all_stopped"] = all_stopped;
        rows.push_back(std::move(row));
    }
    return rows;
}

std::function<void(std::size_t)> make_progress(int index, std::int64_t repeats, std::size_t total) {
    struct State {
        Clock::time_point last{};
        bool reported_any = false;
        long long last_count = -1;
    };
    auto state = std::make_shared<State>();
    return [=](std::size_t done) {
        auto now = Clock::now();
        if (static_cast<long long>(done) == state->last_count) return;
        double since = std::chrono::duration<double>(now - state->last).count();
        if (done >= total || !state->reported_any || since >= kProgressIntervalSeconds) {
            state->last = now;
            state->reported_any = true;
            state->last_count = static_cast<long long>(done);
            log("repetition " + std::to_string(index) + "/" + std::to_string(repeats) + "; shots " +
                std::to_string(done) + "/" + std::to_string(total));
        }
    };
}

std::pair<std::vector<std::string>, Json> resolve_worker(const std::string& worker_command) {
    if (!worker_command.empty()) {
        Json prefix = Json::parse(worker_command, nullptr, false);
        if (prefix.is_discarded()) throw DcDeterminismError("--worker-command must be a JSON array: invalid JSON");
        bool ok = prefix.is_array() && !prefix.empty();
        if (ok) {
            for (const Json& item : prefix) ok = ok && item.is_string();
        }
        if (!ok) throw DcDeterminismError("--worker-command must be a non-empty JSON array of strings");
        Json info = Json::object();
        info["path"] = nullptr;
        info["sha256"] = nullptr;
        info["version"] = nullptr;
        info["manifest_entry"] = nullptr;
        info["command"] = prefix;
        return {prefix.get<std::vector<std::string>>(), info};
    }
    Json info = verify_default_worker();
    return {{info["path"].get<std::string>()}, info};
}

SimulatorTarget resolve_simulator(const std::string& compiler, const std::string& worker_command) {
    if (compiler == kWasmCompiler) {
        WasmOptions options;
        options.check_node = worker_command.empty();
        WasmBundle bundle = verify_wasm_bundle(options);
        if (worker_command.empty()) return {bundle.loader, bundle.command, bundle.worker};
        std::pair<std::vector<std::string>, Json> hook = resolve_worker(worker_command);
        return {bundle.loader, hook.first, hook.second};
    }
    ResolvedLoader loader = loader_provenance(compiler);
    std::pair<std::vector<std::string>, Json> worker = resolve_worker(worker_command);
    return {loader, worker.first, worker.second};
}

}  // namespace dc_determinism
