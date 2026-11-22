#include "capture/collector.hpp"

#include <system_error>

#include "capture/dctr.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"

namespace dc_determinism {

std::int64_t parse_capture_line(const std::string& line, std::int64_t position) {
    std::string text = py_strip(line);
    if (text.compare(0, 3, "ERR") == 0) throw DcDeterminismError("worker reported: " + text);
    std::vector<std::string> fields = py_split(text);
    std::int64_t index = 0;
    std::int64_t size = 0;
    if (fields.size() != 3 || fields[0] != "T" || !py_parse_int(fields[1], index) || index != position ||
        !py_parse_int(fields[2], size) || size < 8) {
        throw DcDeterminismError("unexpected capture line for position " + std::to_string(position) + ": " +
                         py_repr(Json(head_code_points(text, 80))));
    }
    return size;
}

TrajectoryCollector::TrajectoryCollector(const fs::path& directory, const std::string& engine, const Json& shots,
                                         Sink sink)
    : directory_(directory), engine_(engine), shots_(&shots), sink_(std::move(sink)) {}

std::string TrajectoryCollector::request(std::int64_t position) { return "T " + std::to_string(position); }

void TrajectoryCollector::consume(std::int64_t position, const Outcome& outcome, const std::string& extra_line) {
    std::int64_t size = parse_capture_line(extra_line, position);
    fs::path path = directory_ / (std::to_string(position) + ".raw");
    struct Remover {
        fs::path path;
        ~Remover() {
            std::error_code ec;
            fs::remove(path, ec);
        }
    } remover{path};
    std::string raw;
    try {
        raw = read_file(path);
    } catch (const DcDeterminismError& exc) {
        throw DcDeterminismError("cannot read the capture file of position " + std::to_string(position) + ": " + exc.what());
    }
    if (static_cast<std::int64_t>(raw.size()) != size) {
        throw DcDeterminismError("capture file of position " + std::to_string(position) + " has " + std::to_string(raw.size()) +
                         " bytes, the worker announced " + std::to_string(size));
    }
    const Json& shot = (*shots_)[static_cast<std::size_t>(position)];
    std::int64_t frames = 0;
    std::string payload =
        raw_to_payload(raw, shot["shot_id"].get<std::string>(), shot["index"].get<std::int64_t>(), engine_, &frames);
    sink_(position, outcome, std::move(payload), frames);
}

}  // namespace dc_determinism
