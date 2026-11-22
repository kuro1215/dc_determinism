#include "cli/commands/prepare.hpp"

#include <cstdio>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "corpus/prepared.hpp"
#include "json/writer.hpp"

namespace dc_determinism {
namespace cli {

namespace {

std::function<Json()>& provider() {
    static std::function<Json()> instance;
    return instance;
}

}  // namespace

void set_tool_identity_provider(std::function<Json()> value) { provider() = std::move(value); }

Json tool_identity() {
    if (provider()) return provider()();
    Json identity = Json::object();
    identity["implementation"] = "cpp";
    identity["source_tree_sha256"] = "unavailable";
    identity["binary_sha256"] = "unavailable";
    return identity;
}

int command_prepare(const ParsedArgs& args) {
    fs::path logs_dir = args.has("logs_dir") ? path_from_utf8(args.text_or("logs_dir", "")) : default_logs_dir();
    fs::path output = path_from_utf8(args.text_or("output", ""));
    Json document = prepare_corpus(logs_dir, args.integer("limit"), output, tool_identity());

    Json summary = Json::object();
    summary["prepared"] = path_to_utf8(resolve(output));
    summary["shots"] = document["corpus"]["count"];
    summary["prepared_sha256"] = document["prepared_sha256"];
    summary["record_digest"] = document["record_digest"];
    std::string line = json_dumps(summary) + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
