// `benchmark` building blocks: the worker channel with a deadline per reply,
// the `B` reply parser, the engine order and the summary statistics with Python's number types.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "json/json.hpp"
#include "process/process.hpp"

namespace dc_determinism {

extern const char* const kBenchmarkSchema;  // "dc-determinism-benchmark-v1"

// One worker process whose replies are read by a thread, so every reply is awaited against a
// deadline (`Channel`).
class BenchChannel {
public:
    // DcDeterminismError("cannot start worker: ...").
    explicit BenchChannel(const std::vector<std::string>& command);
    ~BenchChannel();
    BenchChannel(const BenchChannel&) = delete;
    BenchChannel& operator=(const BenchChannel&) = delete;

    // Sends one request line and returns the next reply line. `timeout_seconds` <= 0: no limit.
    // Failures kill the worker: DcDeterminismError("worker did not answer '<text>' within N s") /
    // DcDeterminismError("worker ended unexpectedly while processing '<text>' (exit code N)").
    std::string request(const std::string& text, double timeout_seconds);

    void kill();
    // Closes stdin so the worker exits normally; its exit code, or nullopt when it did not exit
    // within 60 s.
    std::optional<std::int64_t> close();
    void dispose();

private:
    std::unique_ptr<Process> process_;
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::string> lines_;
    bool eof_ = false;
};

// `parse_bench_line`: `B <position> <engine> <ns>` -> ns; DcDeterminismError("worker reported: ERR ...") /
// DcDeterminismError("unexpected worker output for benchmark request <position> <engine>: '<text>'").
std::int64_t parse_bench_line(const std::string& line, std::int64_t position, const std::string& engine);

// `engine_order`: forward on even repeats (0-based), reversed on odd repeats.
std::vector<std::string> engine_order(const std::vector<std::string>& engines, std::int64_t repeat);

// `statistics.median(values)` for a list of ints or floats (Json numbers): an element of an odd
// list is returned as is; for an even list `(a + b) / 2` is always a float. Throws DcDeterminismError for
// an empty list.
Json py_median(const std::vector<Json>& values);

// `sum(values)` for ints (exact) or floats (CPython's compensated summation, 3.12 and later).
Json py_sum(const std::vector<Json>& values);

// `summarize(records, engines)`: per engine `{shots, median_of_medians_ns, mean_of_medians_ns,
// total_of_medians_ns}` over `records[*]["median_ns"][engine]`.
Json summarize_benchmark(const Json& records, const std::vector<std::string>& engines);

}  // namespace dc_determinism
