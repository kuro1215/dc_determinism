#include "cli/commands.hpp"

#include <cstring>

#include "cli/commands/capture.hpp"
#include "cli/commands/player.hpp"
#include "cli/commands/prebuild.hpp"
#include "cli/commands/prepare.hpp"
#include "cli/commands/pybind.hpp"
#include "cli/commands/render.hpp"
#include "cli/commands/simulator.hpp"
#include "common/error.hpp"

namespace dc_determinism {
namespace cli {

namespace {

struct Entry {
    const char* path;
    Handler handler;
};

const Entry kHandlers[] = {
    {"prepare", command_prepare},
    {"verify", command_verify},
    {"run simulator", command_run_simulator},
    {"run player", command_run_player},
    {"benchmark", command_benchmark},
    {"capture simulator", command_capture_simulator},
    {"capture player", command_capture_player},
    {"compare simulator", command_compare_simulator},
    {"compare engines", command_compare_engines},
    {"compare pybind", command_compare_pybind},
    {"compare player", command_compare_player},
    {"render trajectory", command_render_trajectory},
    {"prebuild", command_prebuild},
};

}  // namespace

std::string command_path(const ParsedArgs& args) {
    std::string path = args.text_or("command", "");
    if (path == "run") return path + " " + args.text_or("run_command_name", "");
    if (path == "capture") return path + " " + args.text_or("capture_command_name", "");
    if (path == "compare") return path + " " + args.text_or("compare_command_name", "");
    if (path == "render") return path + " " + args.text_or("render_command_name", "");
    return path;
}

Handler find_handler(const std::string& path) {
    for (const Entry& entry : kHandlers) {
        if (path == entry.path) return entry.handler;
    }
    return nullptr;
}

int dispatch(const ParsedArgs& args) {
    std::string path = command_path(args);
    Handler handler = find_handler(path);
    if (handler == nullptr) throw DcDeterminismError("unknown command: " + path);
    return handler(args);
}

}  // namespace cli
}  // namespace dc_determinism
