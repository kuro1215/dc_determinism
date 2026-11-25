// dc_determinism: the C++ determinism evaluator.
//
// Layout (src/dc_determinism):
//   common/   errors, logging, SHA-256, files and atomic no-overwrite writes, repo root, tree hash,
//             tool identity, host environment
//   json/     Python-compatible JSON writer (float repr, ensure_ascii), JSON reading (nlohmann)
//   zip/      ZIP reading and writing (miniz)
//   process/  child processes (CreateProcessW / posix_spawn), process groups
//   cli/      argparse-like parser, the dc_determinism command line (spec.cpp), command dispatch
//   cli/commands/ one source per command
//   render/   SVG plots of captured shots and the collisions inferred from the stone positions (render trajectory)
//   localbuild/ recipe library of `prebuild`: vendored-source identity, receipts and cache of the local builds; the
//             toolchain (compiler discovery and environment) is scripts/build-local.ps1
//   tools/    build-time helpers; tests/ unit tests (CTest)
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "cli/argparse.hpp"
#include "cli/commands.hpp"
#include "cli/commands/prepare.hpp"
#include "cli/spec.hpp"
#include "common/error.hpp"
#include "common/identity.hpp"
#include "common/log.hpp"
#include "common/platform.hpp"

int main(int argc, char** argv) {
    using namespace dc_determinism;
    std::vector<std::string> args = utf8_arguments(argc, argv);
    if (!args.empty()) args.erase(args.begin());

    // `--version` is answered before any command is parsed.
    if (!args.empty() && args[0] == "--version") {
        std::printf("%s\n", version_line().c_str());
        return 0;
    }
    // Developer switch (for scripted checks of the argument table): parse and validate the arguments
    // only, exit 0 when they are accepted and 2 on a usage error.
    bool parse_only = !args.empty() && args[0] == "--parse-only";
    if (parse_only) args.erase(args.begin());

    cli::set_tool_identity_provider([] {
        Json identity = Json::object();
        identity["implementation"] = "cpp";
        identity["source_tree_sha256"] = source_tree_sha256();
        identity["binary_sha256"] = binary_sha256();
        return identity;
    });

    try {
        std::unique_ptr<cli::Parser> parser = cli::build_parser();
        cli::ParsedArgs parsed = parser->parse(args);
        if (parse_only) return 0;
        return cli::dispatch(parsed);
    } catch (const cli::ParseExit& exit) {
        return exit.code;
    } catch (const DcDeterminismError& error) {
        log(std::string("error: ") + error.what());
        return 2;
    } catch (const std::exception& error) {
        log(std::string("error: unexpected ") + error.what());
        return 2;
    }
}
