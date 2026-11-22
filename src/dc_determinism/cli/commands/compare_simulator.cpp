#include "capture/compare_traj.hpp"
#include "cli/commands/simulator.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/compare.hpp"

namespace dc_determinism {
namespace cli {

int command_compare_simulator(const ParsedArgs& args) {
    std::string path_a = args.text_or("a", "");
    std::string path_b = args.text_or("b", "");
    fs::path output = path_from_utf8(args.text_or("output", ""));
    refuse_existing(output);
    bool archive_a = is_archive(path_from_utf8(path_a));
    bool archive_b = is_archive(path_from_utf8(path_b));
    if (archive_a != archive_b) {
        throw DcDeterminismError("A and B must be of the same kind: two `run simulator` result JSON files or two `capture` "
                         "archive ZIP files");
    }
    if (archive_a) {
        return compare_archives(path_a, path_b, output, args.integer("workers"));
    }
    Json a = load_result(path_a);
    Json b = load_result(path_b);
    Json report = compare_results(a, b, sha256_file(path_from_utf8(path_a)), sha256_file(path_from_utf8(path_b)));
    write_json_new(output, report);
    log(report["classification"].get<std::string>() + ": total " + std::to_string(report["total"].get<std::int64_t>()) +
        ", trajectory mismatches " + std::to_string(report["trajectory"]["mismatches"].get<std::int64_t>()) + "/" +
        std::to_string(report["trajectory"]["compared"].get<std::int64_t>()) + ", final mismatches " +
        std::to_string(report["final"]["mismatches"].get<std::int64_t>()) + "/" +
        std::to_string(report["final"]["compared"].get<std::int64_t>()) + ", excluded " +
        std::to_string(report["excluded"].get<std::int64_t>()));
    return report["identical"].get<bool>() ? 0 : 1;
}

}  // namespace cli
}  // namespace dc_determinism
