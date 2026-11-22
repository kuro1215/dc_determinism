// The log comparison of `run simulator --compare-logs`: reads the recorded trajectories, asks the workers for the sampled boards
// (`O` requests) and computes the metrics on a pool of threads.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "corpus/catalogue.hpp"
#include "json/json.hpp"
#include "logs/log_compare.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {

// `query_seconds_per_frame`: the simulator step as float32 bits (8 hex digits) and the exactly
// widened double. `prefix` is the worker command prefix.
std::pair<std::string, double> query_seconds_per_frame(const std::vector<std::string>& prefix,
                                                       const std::string& loader, const std::string& engine,
                                                       double timeout_seconds = 120.0);

// (frame count, frame duration) of a recorded trajectory.
struct FrameInfo {
    std::size_t frames = 0;
    double duration = 0.0;
};

class LogObserver;

// Owns what the log reading needs: the catalogue (read only) and the settings. `shots` are the
// prepared.json shot entries; `Catalogue::rows()` is indexed by their `index`.
class LogEvaluator {
public:
    LogEvaluator(const Catalogue& catalogue, const Json& shots, double sim_spf, const LogSettings& settings,
                 int workers);

    const Catalogue& catalogue() const { return *catalogue_; }
    const Json& shots() const { return *shots_; }
    double sim_spf() const { return sim_spf_; }
    const LogSettings& settings() const { return settings_; }
    int workers() const { return workers_; }

    // (frame count, frame duration) of every shot's recorded trajectory, read on `workers`
    // threads; the error of the first failing shot (in order) is reported.
    std::vector<FrameInfo> frame_infos() const;

    // The observer of repetition 1; valid while this evaluator lives.
    std::unique_ptr<LogObserver> observer(const std::vector<FrameInfo>& infos) const;

private:
    const Catalogue* catalogue_;
    const Json* shots_;
    double sim_spf_;
    LogSettings settings_;
    int workers_;
};

// Repetition-1 hook of `run_repetition`: sends `O` requests and queues the metrics.
class LogObserver : public RunObserver {
public:
    LogObserver(const LogEvaluator& evaluator, const std::vector<FrameInfo>& infos);
    ~LogObserver() override;

    std::string request(std::int64_t position) override;
    void consume(std::int64_t position, const Outcome& outcome, const std::string& extra_line) override;
    void finish(double remaining_seconds) override;
    void cancel() override;

    // The metrics by position (null: none); complete after `finish` returned.
    const std::vector<Json>& metrics() const { return metrics_; }

private:
    struct Task {
        std::int64_t position;
        std::size_t count;
        std::string payload;
    };

    std::vector<std::int64_t> steps(std::int64_t position) const;
    void work();
    void record_error(std::int64_t position, const std::string& what);

    const LogEvaluator* evaluator_;
    std::vector<FrameInfo> infos_;
    std::vector<Json> metrics_;
    std::size_t slots_;

    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Task> queue_;
    std::size_t outstanding_ = 0;  // queued + running tasks
    bool cancelled_ = false;
    bool stopping_ = false;
    std::string error_;
    bool failed_ = false;
    std::vector<std::thread> threads_;
};

}  // namespace dc_determinism
