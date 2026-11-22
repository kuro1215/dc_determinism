#include "sim/bench.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "common/error.hpp"
#include "common/text.hpp"

namespace dc_determinism {

const char* const kBenchmarkSchema = "dc-determinism-benchmark-v1";

BenchChannel::BenchChannel(const std::vector<std::string>& command) {
    StartOptions options;
    options.argv = command;
    options.pipe_stdin = true;
    options.pipe_stdout = true;
    try {
        process_ = Process::start(options);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("cannot start worker: ") + error.what());
    }
    Process* process = process_.get();
    reader_ = std::thread([this, process] {
        std::string line;
        while (process->read_line(line)) {
            std::lock_guard<std::mutex> lock(mutex_);
            lines_.push_back(line);
            wake_.notify_all();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        eof_ = true;
        wake_.notify_all();
    });
}

BenchChannel::~BenchChannel() { dispose(); }

std::string BenchChannel::request(const std::string& text, double timeout_seconds) {
    process_->write_stdin(text + "\n");  // a dead worker is explained by its ERR line (or EOF)
    std::unique_lock<std::mutex> lock(mutex_);
    auto ready = [this] { return !lines_.empty() || eof_; };
    bool answered = true;
    if (timeout_seconds > 0) {
        answered = wake_.wait_for(lock, std::chrono::duration<double>(timeout_seconds), ready);
    } else {
        wake_.wait(lock, ready);
    }
    if (!answered) {
        lock.unlock();
        kill();
        throw DcDeterminismError("worker did not answer " + py_repr(Json(text)) + " within " + format_g(timeout_seconds) +
                         " s");
    }
    if (lines_.empty()) {
        lock.unlock();
        std::string code = process_->wait_for(0.2) ? std::to_string(process_->exit_code()) : std::string("None");
        throw DcDeterminismError("worker ended unexpectedly while processing " + py_repr(Json(text)) + " (exit code " + code +
                         ")");
    }
    std::string line = lines_.front();
    lines_.pop_front();
    return line;
}

void BenchChannel::kill() {
    if (process_ && process_->running()) process_->kill();
    if (process_) process_->wait_for(30.0);
}

std::optional<std::int64_t> BenchChannel::close() {
    process_->close_stdin();
    if (!process_->wait_for(60.0)) return std::nullopt;
    return process_->exit_code();
}

void BenchChannel::dispose() {
    if (!process_) return;
    kill();
    if (reader_.joinable()) reader_.join();
    process_.reset();
}

std::int64_t parse_bench_line(const std::string& line, std::int64_t position, const std::string& engine) {
    std::string text = py_strip(line);
    if (text.compare(0, 3, "ERR") == 0) throw DcDeterminismError("worker reported: " + text);
    std::vector<std::string> fields = py_split(text);
    std::int64_t index = 0, nanoseconds = 0;
    if (fields.size() != 4 || fields[0] != "B" || !py_parse_int(fields[1], index) || index != position ||
        fields[2] != engine || !py_parse_int(fields[3], nanoseconds) || nanoseconds < 0) {
        throw DcDeterminismError("unexpected worker output for benchmark request " + std::to_string(position) + " " + engine +
                         ": " + py_repr(Json(text)));
    }
    return nanoseconds;
}

std::vector<std::string> engine_order(const std::vector<std::string>& engines, std::int64_t repeat) {
    std::vector<std::string> order = engines;
    if (repeat % 2 != 0) std::reverse(order.begin(), order.end());
    return order;
}

namespace {

double number(const Json& value) { return value.is_number_integer() ? static_cast<double>(value.get<std::int64_t>())
                                                                    : value.get<double>(); }

}  // namespace

Json py_median(const std::vector<Json>& values) {
    if (values.empty()) throw DcDeterminismError("no median for empty data");
    std::vector<Json> sorted = values;
    std::stable_sort(sorted.begin(), sorted.end(), [](const Json& a, const Json& b) { return number(a) < number(b); });
    std::size_t n = sorted.size();
    if (n % 2 == 1) return sorted[n / 2];
    const Json& a = sorted[n / 2 - 1];
    const Json& b = sorted[n / 2];
    if (a.is_number_integer() && b.is_number_integer()) {
        return Json(static_cast<double>(a.get<std::int64_t>() + b.get<std::int64_t>()) / 2.0);
    }
    return Json((number(a) + number(b)) / 2.0);
}

Json py_sum(const std::vector<Json>& values) {
    bool all_ints = true;
    for (const Json& value : values) all_ints = all_ints && value.is_number_integer();
    if (all_ints) {
        std::int64_t total = 0;
        for (const Json& value : values) total += value.get<std::int64_t>();
        return Json(total);
    }
    // The leading ints are added exactly; from the first float on the sum is a float, and floats
    // are added with Neumaier's compensation (sum() of CPython 3.12 and later); an int in the float
    // phase is added plainly.
    std::size_t i = 0;
    std::int64_t exact = 0;
    while (i < values.size() && values[i].is_number_integer()) exact += values[i++].get<std::int64_t>();
    double total = static_cast<double>(exact) + values[i++].get<double>();
    double compensation = 0.0;
    for (; i < values.size(); ++i) {
        if (values[i].is_number_integer()) {
            total += static_cast<double>(values[i].get<std::int64_t>());
            continue;
        }
        double x = values[i].get<double>();
        double t = total + x;
        if (std::fabs(total) >= std::fabs(x)) {
            compensation += (total - t) + x;
        } else {
            compensation += (x - t) + total;
        }
        total = t;
    }
    if (compensation != 0.0 && std::isfinite(compensation)) total += compensation;
    return Json(total);
}

Json summarize_benchmark(const Json& records, const std::vector<std::string>& engines) {
    Json out = Json::object();
    for (const std::string& engine : engines) {
        std::vector<Json> medians;
        for (const Json& record : records) medians.push_back(record["median_ns"][engine]);
        if (medians.empty()) throw DcDeterminismError("no shots to summarize");
        Json total = py_sum(medians);
        Json entry = Json::object();
        entry["shots"] = static_cast<std::int64_t>(medians.size());
        entry["median_of_medians_ns"] = py_median(medians);
        entry["mean_of_medians_ns"] = number(total) / static_cast<double>(medians.size());
        entry["total_of_medians_ns"] = total;
        out[engine] = entry;
    }
    return out;
}

}  // namespace dc_determinism
