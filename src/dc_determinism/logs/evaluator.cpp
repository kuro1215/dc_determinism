#include "logs/evaluator.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "common/error.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "json/pyparse.hpp"
#include "process/process.hpp"

namespace dc_determinism {

namespace {

constexpr auto kWait = std::chrono::milliseconds(200);
constexpr double kProgressIntervalSeconds = 5.0;

using Clock = std::chrono::steady_clock;

// What a comparison needs of a shot log: the recorded trajectory.
const JsonKeepTree& trajectory_keep() {
    static const JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "trajectory"});
        return tree;
    }();
    return keep;
}

std::int64_t shot_index(const Json& shots, std::int64_t position) {
    return shots[static_cast<std::size_t>(position)]["index"].get<std::int64_t>();
}

std::string shot_id(const Json& shots, std::int64_t position) {
    return shots[static_cast<std::size_t>(position)]["shot_id"].get<std::string>();
}

bool is_lower_hex8(const std::string& text) {
    if (text.size() != 8) return false;
    return std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

}  // namespace

std::pair<std::string, double> query_seconds_per_frame(const std::vector<std::string>& prefix,
                                                       const std::string& loader, const std::string& engine,
                                                       double timeout_seconds) {
    StartOptions options;
    options.argv = prefix;
    options.argv.push_back("--loader");
    options.argv.push_back(loader);
    options.argv.push_back("--engine");
    options.argv.push_back(engine);
    options.argv.push_back("--seconds-per-frame");
    RunResult result;
    try {
        result = run_capture(options, std::string(), timeout_seconds);
    } catch (const std::exception& exc) {
        throw DcDeterminismError(std::string("cannot query seconds per frame: ") + exc.what());
    }
    if (result.timed_out) {
        throw DcDeterminismError("cannot query seconds per frame: timed out after " + format_g(timeout_seconds) + " seconds");
    }
    std::string text = py_strip(result.out);
    if (result.exit_code != 0 || !is_lower_hex8(text)) {
        std::string detail = head_code_points(py_strip(result.out + result.err), 200);
        throw DcDeterminismError("seconds-per-frame query failed (exit " + std::to_string(result.exit_code) +
                         "): " + py_repr(Json(detail)));
    }
    std::uint32_t bits = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    double widened = static_cast<double>(value);
    if (!std::isfinite(widened) || widened <= 0) {
        throw DcDeterminismError("invalid seconds per frame reported by the worker: " + text);
    }
    return {text, widened};
}

// ---------------------------------------------------------------------------------- evaluator

LogEvaluator::LogEvaluator(const Catalogue& catalogue, const Json& shots, double sim_spf,
                           const LogSettings& settings, int workers)
    : catalogue_(&catalogue), shots_(&shots), sim_spf_(sim_spf), settings_(settings), workers_(std::max(1, workers)) {}

std::vector<FrameInfo> LogEvaluator::frame_infos() const {
    const std::size_t total = shots_->size();
    std::vector<FrameInfo> infos(total);
    std::vector<std::string> errors(total);
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::size_t> first_failed{total};
    std::atomic<int> running{0};

    int thread_count = static_cast<int>(std::max<std::size_t>(1, std::min<std::size_t>(workers_, total)));
    std::vector<std::thread> threads;
    running = thread_count;
    for (int t = 0; t < thread_count; ++t) {
        threads.emplace_back([&] {
            ShotSource source(*catalogue_);
            for (;;) {
                std::size_t i = next.fetch_add(1);
                if (i >= total || i > first_failed.load()) break;
                try {
                    LoadedShot shot = source.load(source.locate(shot_index(*shots_, static_cast<std::int64_t>(i))),
                                                  &trajectory_keep());
                    Trajectory trajectory = trajectory_from_log(shot.log);
                    infos[i].frames = trajectory.frames.size();
                    infos[i].duration = trajectory.seconds_per_frame;
                } catch (const std::exception& exc) {
                    errors[i] = "cannot read the recorded trajectory of " + shot_id(*shots_, static_cast<std::int64_t>(i)) +
                                ": " + exc.what();
                    std::size_t seen = first_failed.load();
                    while (i < seen && !first_failed.compare_exchange_weak(seen, i)) {
                    }
                }
                done.fetch_add(1);
            }
            running.fetch_sub(1);
        });
    }
    auto last = Clock::now();
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (std::chrono::duration<double>(Clock::now() - last).count() >= kProgressIntervalSeconds) {
            log("recorded trajectories " + std::to_string(done.load()) + "/" + std::to_string(total));
            last = Clock::now();
        }
    }
    for (std::thread& thread : threads) thread.join();
    std::size_t failed = first_failed.load();
    if (failed < total) throw DcDeterminismError(errors[failed]);
    return infos;
}

std::unique_ptr<LogObserver> LogEvaluator::observer(const std::vector<FrameInfo>& infos) const {
    return std::unique_ptr<LogObserver>(new LogObserver(*this, infos));
}

// ---------------------------------------------------------------------------------- observer

LogObserver::LogObserver(const LogEvaluator& evaluator, const std::vector<FrameInfo>& infos)
    : evaluator_(&evaluator),
      infos_(infos),
      metrics_(infos.size()),
      slots_(static_cast<std::size_t>(std::max(4, 4 * evaluator.workers()))) {
    for (int i = 0; i < evaluator.workers(); ++i) threads_.emplace_back([this] { work(); });
}

LogObserver::~LogObserver() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        cancelled_ = true;
    }
    condition_.notify_all();
    for (std::thread& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
}

std::vector<std::int64_t> LogObserver::steps(std::int64_t position) const {
    if (position < 0 || static_cast<std::size_t>(position) >= infos_.size()) {
        throw DcDeterminismError("log comparison: unknown position " + std::to_string(position));
    }
    const FrameInfo& info = infos_[static_cast<std::size_t>(position)];
    return requested_steps(frame_steps(info.frames, info.duration, evaluator_->sim_spf()));
}

std::string LogObserver::request(std::int64_t position) {
    std::vector<std::int64_t> list = steps(position);
    std::string text = "O " + std::to_string(position) + " ";
    if (list.empty()) return text + "-";
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i != 0) text += ",";
        text += std::to_string(list[i]);
    }
    return text;
}

void LogObserver::consume(std::int64_t position, const Outcome& outcome, const std::string& extra_line) {
    std::pair<std::int64_t, std::string> sampled = decode_s_line(extra_line, position);
    std::int64_t count = sampled.first;
    std::int64_t executed = outcome.steps;
    std::int64_t expected = 0;
    for (std::int64_t step : steps(position)) expected += step <= executed ? 1 : 0;
    if (count != expected) {
        throw DcDeterminismError("position " + std::to_string(position) + ": worker returned " + std::to_string(count) +
                         " sampled boards, expected " + std::to_string(expected) + " for " +
                         std::to_string(executed) + " executed steps");
    }
    std::size_t wanted = kBoardSize * (1 + static_cast<std::size_t>(count));
    if (sampled.second.size() != wanted) {
        throw DcDeterminismError("position " + std::to_string(position) + ": sampled-board payload has " +
                         std::to_string(sampled.second.size()) + " bytes, expected " + std::to_string(wanted));
    }
    if (outcome.status != "stopped") return;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (outstanding_ >= slots_) {
            if (cancelled_ || failed_) throw DcDeterminismError(failed_ ? error_ : std::string("log comparison cancelled"));
            condition_.wait_for(lock, kWait);
        }
        if (cancelled_ || failed_) throw DcDeterminismError(failed_ ? error_ : std::string("log comparison cancelled"));
        ++outstanding_;
        queue_.push_back(Task{position, static_cast<std::size_t>(count), std::move(sampled.second)});
    }
    condition_.notify_all();
}

void LogObserver::record_error(std::int64_t position, const std::string& what) {
    // The caller holds the mutex.
    if (!failed_) {
        failed_ = true;
        error_ = "log comparison failed for " + shot_id(evaluator_->shots(), position) + ": " + what;
    }
}

void LogObserver::work() {
    ShotSource source(evaluator_->catalogue());
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        Json metrics;
        std::string failure;
        bool dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            dropped = cancelled_ || failed_;
        }
        if (!dropped) {
            try {
                LoadedShot item = source.load(source.locate(shot_index(evaluator_->shots(), task.position)),
                                              &trajectory_keep());
                Trajectory trajectory = trajectory_from_log(item.log);
                std::vector<std::int64_t> steps = frame_steps(trajectory.frames.size(), trajectory.seconds_per_frame,
                                                              evaluator_->sim_spf());
                std::vector<std::int64_t> requested = requested_steps(steps);
                if (task.count > requested.size()) {
                    throw DcDeterminismError("worker returned " + std::to_string(task.count) + " sampled boards for " +
                                     std::to_string(requested.size()) + " steps");
                }
                std::pair<Board, std::vector<Board>> boards = decode_payload(task.payload, task.count);
                SampledBoards sampled;
                for (std::size_t i = 0; i < boards.second.size(); ++i) sampled.emplace(requested[i], boards.second[i]);
                metrics = shot_metrics(item.log, item.fgz, boards.first, trajectory.frames, steps, sampled,
                                       evaluator_->settings().tolerance);
            } catch (const std::exception& exc) {
                failure = exc.what();
                if (failure.empty()) failure = "unknown error";
                dropped = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!failure.empty()) {
                record_error(task.position, failure);
            } else if (!dropped) {
                metrics_[static_cast<std::size_t>(task.position)] = std::move(metrics);
            }
            --outstanding_;
        }
        condition_.notify_all();
    }
}

void LogObserver::finish(double remaining_seconds) {
    bool has_deadline = remaining_seconds >= 0;
    auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                                       has_deadline ? remaining_seconds : 0.0));
    std::unique_lock<std::mutex> lock(mutex_);
    while (outstanding_ > 0 && !failed_) {
        if (has_deadline && Clock::now() >= deadline) {
            throw DcDeterminismError("timed out waiting for log comparison metrics (" + std::to_string(outstanding_) +
                             " shots pending)");
        }
        condition_.wait_for(lock, kWait);
    }
    if (failed_) throw DcDeterminismError(error_);
}

void LogObserver::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_ = true;
    }
    condition_.notify_all();
}

}  // namespace dc_determinism
