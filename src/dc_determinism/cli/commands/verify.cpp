#include <cstdio>
#include <exception>

#include "cli/commands/simulator.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/text.hpp"
#include "json/writer.hpp"
#include "sim/provenance.hpp"
#include "sim/wasm.hpp"

namespace dc_determinism {
namespace cli {

std::string short_reason(const std::string& message) {
    std::string text = py_strip(message);
    std::size_t line_end = text.find_first_of("\r\n");
    std::string first = line_end == std::string::npos ? text : text.substr(0, line_end);
    if (first.empty()) first = "DcDeterminismError";
    std::size_t split = first.find(": ");
    std::string head = split == std::string::npos ? first : first.substr(0, split);
    while (!head.empty() && head.back() == ':') head.pop_back();
    head = py_strip(head);
    return head.empty() ? first : head;
}

namespace {

void print_line(const std::string& text) {
    std::string line = text + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

}  // namespace

int command_verify(const ParsedArgs& args) {
    std::string compiler = args.text_or("compiler", "");
    std::string label = compiler.empty() ? "worker" : compiler;
    bool verbose = args.flag("verbose");
    Json out = Json::object();
    try {
        if (compiler == kWasmCompiler) {
            // The wasm worker (through node) replaces the native one.
            WasmBundle bundle = verify_wasm_bundle();
            worker_self_test(bundle.command);
            out["verified"] = true;
            Json record = bundle.worker;
            record["self_test"] = "OK";
            out["worker"] = record;
            out["loader"] = bundle.loader.info;
        } else {
            Json worker = verify_default_worker();
            worker_self_test(path_from_utf8(worker["path"].get<std::string>()));
            out["verified"] = true;
            Json record = worker;
            record["self_test"] = "OK";
            out["worker"] = record;
            if (!compiler.empty()) {
                ResolvedLoader loader = loader_provenance(compiler);
                out["loader"] = loader.info;
            }
        }
    } catch (const std::exception& exc) {
        if (verbose) throw;
        print_line(label + ": unverified (" + short_reason(exc.what()) + ")");
        return 2;
    }
    if (verbose) {
        print_line(json_dumps(out, DumpOptions::pretty(2)));
    } else {
        print_line(label + ": verified");
    }
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
