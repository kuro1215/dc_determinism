// The observer of `run_repetition` that sends the worker's `T <pos>` capture requests (the full raw
// trajectory of a shot) and converts each capture file to a `dctr-v1` payload; shared by `capture simulator`
// and the Path A part of `compare pybind`.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common/platform.hpp"
#include "json/json.hpp"
#include "sim/runner.hpp"

namespace dc_determinism {

// `T <pos> <file_bytes>` -> the size of the capture file.
std::int64_t parse_capture_line(const std::string& line, std::int64_t position);

// Sends `T` requests, converts each capture file and passes `(position, outcome, payload, frames)` to the
// sink; the `.raw` file is deleted either way.
class TrajectoryCollector : public RunObserver {
public:
    using Sink = std::function<void(std::int64_t, const Outcome&, std::string&&, std::int64_t)>;

    TrajectoryCollector(const fs::path& directory, const std::string& engine, const Json& shots, Sink sink);

    std::string request(std::int64_t position) override;
    void consume(std::int64_t position, const Outcome& outcome, const std::string& extra_line) override;
    void finish(double) override {}
    void cancel() override {}

private:
    fs::path directory_;
    std::string engine_;
    const Json* shots_;
    Sink sink_;
};

}  // namespace dc_determinism
