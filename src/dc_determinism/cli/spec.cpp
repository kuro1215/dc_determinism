#include "cli/spec.hpp"

#include <algorithm>

#include "common/platform.hpp"
#include "sim/compilers.hpp"

namespace dc_determinism {
namespace cli {

namespace {

const char* const kWasmHelp = "wasm: the prebuilt WebAssembly worker (prebuilt/worker/wasm, run with node)";

// The simulator commands also take `wasm`: the prebuilt WebAssembly worker, not a native loader. The
// `*-nofma` ids (sim/compilers.hpp) are parser choices on every platform; outside linux-arm64 the
// command exits 2 when it runs.
const std::vector<std::string>& kCompilers = simulator_compiler_choices();

ValueType integer_type(const std::string& name, std::int64_t minimum) {
    ValueType type;
    type.kind = ValueKind::Int;
    type.name = name;
    type.minimum = static_cast<double>(minimum);
    return type;
}

ValueType float_type(const std::string& name, double minimum) {
    ValueType type;
    type.kind = ValueKind::Float;
    type.name = name;
    type.minimum = minimum;
    return type;
}

// A value option (`--flag VALUE`).
Argument value_option(const std::string& flag, const std::string& help = std::string()) {
    Argument argument;
    argument.flags = {flag};
    argument.help = help;
    return argument;
}

Argument hidden_option(const std::string& flag) {
    Argument argument = value_option(flag);
    argument.hidden = true;
    return argument;
}

Argument bool_option(const std::string& flag, const std::string& help) {
    Argument argument = value_option(flag, help);
    argument.store_true = true;
    return argument;
}

Argument bounded_int(const std::string& flag, std::int64_t minimum, std::int64_t default_value,
                     const std::string& help = std::string()) {
    Argument argument = value_option(flag, help);
    argument.type = integer_type(flag, minimum);
    argument.default_int = default_value;
    return argument;
}

// An integer option without a default (`has()` tells whether it was given).
Argument optional_int(const std::string& flag, std::int64_t minimum, const std::string& help = std::string()) {
    Argument argument = value_option(flag, help);
    argument.type = integer_type(flag, minimum);
    return argument;
}

Argument bounded_float(const std::string& flag, double minimum, std::optional<double> default_value,
                       const std::string& help = std::string()) {
    Argument argument = value_option(flag, help);
    argument.type = float_type(flag, minimum);
    argument.default_float = default_value;
    return argument;
}

Argument choice_option(const std::string& flag, std::vector<std::string> choices, bool required,
                       const std::string& help = std::string()) {
    Argument argument = value_option(flag, help);
    argument.choices = std::move(choices);
    argument.required = required;
    return argument;
}

Argument required_option(const std::string& flag, const std::string& help) {
    Argument argument = value_option(flag, help);
    argument.required = true;
    return argument;
}

Argument positional(const std::string& dest, const std::string& metavar) {
    Argument argument;
    argument.dest = dest;
    argument.metavar = metavar;
    return argument;
}

void add_loader_options(Parser& parser, std::vector<std::string> choices = kCompilers, bool required = true) {
    parser.add_group("loader");
    bool wasm = std::find(choices.begin(), choices.end(), "wasm") != choices.end();
    std::string help = "prebuilt loader in prebuilt/loaders (verified against its manifest)";
    if (wasm) help += "; " + std::string(kWasmHelp);
    parser.add(choice_option("--compiler", std::move(choices), required, help));
    parser.use_group("");
}

// `run player` repeats the player and writes a result JSON; `capture player` runs it once and keeps
// its output. The two share the selection and verification of the player.
void add_player_options(Parser& parser, bool capture) {
    parser.add(choice_option("--player", {"normal_dist", "nd2"}, true));
    parser.add(choice_option("--compiler", player_compiler_choices(), true,
                             "prebuilt player binary in prebuilt/players"));
    if (!capture) parser.add(bounded_int("--repeats", 2, 2, "fresh-process repetitions, >= 2 (default: 2)"));
    parser.add(bounded_int("--samples", 640, 4096, "samples per block, 640..1000000 (default: 4096)"));
    if (capture) {
        parser.add(value_option("--expect",
                                "a reproducible `run player` result of the same player, compiler and "
                                "samples; exit 1 when the captured output differs from its output_sha256"))
            .metavar = "RESULT.json";
        parser.add(required_option("--output", "player output .bin (never overwritten)"));
    } else {
        parser.add(required_option("--output", "result JSON (never overwritten)"));
    }
    parser.add(bounded_float("--timeout", 0, 0.0,
                             capture ? "seconds for the player run, 0 = no limit (default: 0)"
                                     : "seconds per repetition, 0 = no limit (default: 0)"));
    parser.add(hidden_option("--player-command"));
}

}  // namespace

std::unique_ptr<Parser> build_parser() {
    const std::int64_t cpus = cpu_count();
    std::unique_ptr<Parser> root(new Parser(
        "dc_determinism", "Raw-state C++ evaluator for the cross-environment determinism experiment."));
    root->add(bool_option("--version", "show the tool identity (source tree hash) and exit"));
    root->add_subparsers("command");

    Parser& prepare = root->add_command("prepare", "freeze the shot corpus into a prepared directory");
    prepare.add(value_option("--logs-dir", "directory with the game log ZIPs (default: <repo>/logs)"));
    prepare.add(bounded_int("--limit", 0, 0, "number of shots, 0 = all (default: 0)"));
    prepare.add(required_option("--output", "new directory for prepared.bin/.json"));

    Parser& verify = root->add_command("verify", "check the worker binary and optionally a loader");
    verify.add(choice_option("--compiler", kCompilers, false,
                             "also check the prebuilt loader; " + std::string(kWasmHelp)));
    verify.add(bool_option("--verbose",
                           "print the full JSON record (and errors in full) instead of "
                           "'<compiler>: verified|unverified (reason)'"));

    Parser& run = root->add_command("run", "run the simulator or a player repeatedly");
    run.add_subparsers("run_command_name");
    Parser& rsim = run.add_command("simulator", "run the C++ workers repeatedly and write a result JSON");
    rsim.add_group("corpus (exactly one)");
    rsim.add(value_option("--logs-dir", "prepare from this logs directory first"));
    rsim.add(value_option("--prepared", "directory written by `prepare`"));
    rsim.use_group("");
    rsim.add(bounded_int("--limit", 0, 0, "with --logs-dir: number of shots, 0 = all (default: 0)"));
    add_loader_options(rsim);
    rsim.add(choice_option("--engine", {"fcv1", "fcv1plus"}, true));
    rsim.add(bounded_int("--repeats", 2, 2, "repetitions with fresh processes, >= 2 (default: 2)"));
    rsim.add(bounded_int("--workers", 1, cpus, "worker processes per repetition (default: logical CPUs)"));
    rsim.add(bounded_int("--max-steps", 1, 60000));
    rsim.add(bounded_float("--timeout", 0, 0.0, "seconds per repetition, 0 = no limit (default: 0)"));
    rsim.add(hidden_option("--worker-command"));
    rsim.add_group("log comparison",
                   "compare repetition 0 with the recorded trajectories; needs --logs-dir (also together "
                   "with --prepared); never changes the exit code");
    rsim.add(bool_option("--compare-logs", "compare final/trajectory boards with the shot logs"));
    rsim.add(choice_option("--compare-logs-basis", {"trajectory-last", "finish"}, false,
                           "primary basis for matched_basis (default: trajectory-last)"));
    rsim.add(bool_option("--compare-logs-basis-fallback",
                         "also try the other basis when the primary one does not match"));
    rsim.add(bounded_float("--compare-logs-tolerance", 0, std::nullopt,
                           "max axis error in metres counted as a match (default: 0.01)"));
    rsim.use_group("");
    rsim.add(required_option("--output", "result JSON (never overwritten)"));
    Parser& rplayer = run.add_command("player", "run a player in fresh processes and write a result JSON");
    add_player_options(rplayer, false);

    Parser& bench = root->add_command("benchmark", "serial timing of the engines on the corpus");
    bench.add_group("corpus (exactly one)");
    bench.add(value_option("--logs-dir", "prepare from this logs directory first"));
    bench.add(value_option("--prepared", "directory written by `prepare`"));
    bench.use_group("");
    bench.add(bounded_int("--limit", 0, 0, "with --logs-dir: number of shots, 0 = all (default: 0)"));
    add_loader_options(bench);
    bench.add(bounded_int("--repeats", 1, 5, "measured repeats per engine and shot (default: 5)"));
    bench.add(bounded_float("--timeout", 0, 0.0, "seconds per single measurement, 0 = no limit (default: 0)"));
    bench.add(hidden_option("--worker-command"));
    bench.add(required_option("--output", "result JSON (never overwritten)"));

    Parser& capture = root->add_command("capture", "store raw trajectories (simulator) or the output (player)");
    capture.add_subparsers("capture_command_name");
    Parser& cap = capture.add_command("simulator", "store full raw trajectories of selected shots");
    cap.add(required_option("--prepared", "directory written by `prepare`"));
    cap.add(choice_option("--engine", {"fcv1", "fcv1plus", "pybind-py27", "pybind-py36"}, true,
                          "fcv1/fcv1plus: the C API through the worker; pybind-py27/pybind-py36: the v2 "
                          "recording binaries of prebuilt/pybind (linux-amd64, dc_determinism-cpu image)"));
    add_loader_options(cap, kCompilers, false);
    cap.add(value_option("--shots",
                         "the shots to capture: a `compare simulator` report JSON (shots whose digests "
                         "differ) or a newline-separated shot-id file (detected by content)"))
        .metavar = "SRC";
    cap.add(bool_option("--all", "every shot (large output)"));
    cap.add_exclusive({"shots", "all"}, true);
    cap.add(value_option("--logs-dir", "pybind engines: the logs behind --prepared"));
    cap.add(value_option("--expect",
                         "C API engines: result of the same contract and loader; exit 1 when a captured "
                         "trajectory or final digest differs from it"))
        .metavar = "RESULT.json";
    cap.add(bounded_int("--workers", 1, cpus, "worker processes (default: logical CPUs)"));
    cap.add(bounded_int("--max-steps", 1, 60000));
    cap.add(bounded_float("--timeout", 0, 0.0,
                          "seconds for the whole capture (pybind: per recording call), 0 = no limit "
                          "(default: 0)"));
    cap.add(hidden_option("--worker-command"));
    cap.add(hidden_option("--pybind-worker-command"));
    cap.add(required_option("--output", "capture archive .zip (never overwritten)"));
    Parser& cplay = capture.add_command("player", "run a player once and keep its output (.bin)");
    add_player_options(cplay, true);

    Parser& cmp = root->add_command("compare", "compare results, captures, engines or environments");
    cmp.add_subparsers("compare_command_name");
    Parser& csim = cmp.add_command("simulator",
                                   "two `run simulator` result JSON files, or two `capture simulator` archive ZIP files");
    csim.add(positional("a", "A"));
    csim.add(positional("b", "B"));
    csim.add(bounded_int("--workers", 1, cpus, "ZIP mode: comparison processes (default: logical CPUs)"));
    csim.add(required_option("--output", "comparison JSON (never overwritten)"));

    Parser& ceng = cmp.add_command("engines", "FCV1 against FCV1Plus in lockstep");
    ceng.add(required_option("--prepared", "directory written by `prepare`"));
    add_loader_options(ceng);
    ceng.add(value_option("--logs-dir",
                          "also compare both final boards with the recorded logs (needs the logs behind "
                          "--prepared)"));
    ceng.add(bounded_float("--compare-logs-tolerance", 0, std::nullopt,
                           "max axis error in metres counted as a match (default: 0.01)"));
    ceng.add(bounded_int("--workers", 1, cpus, "worker processes (default: logical CPUs)"));
    ceng.add(bounded_int("--max-steps", 1, 60000));
    ceng.add(bounded_float("--timeout", 0, 0.0, "seconds for the whole run, 0 = no limit (default: 0)"));
    ceng.add(hidden_option("--worker-command"));
    ceng.add(required_option("--output", "report JSON (never overwritten)"));

    Parser& cpy = cmp.add_command("pybind", "Path A (C API) against Path B (official pybind), linux-amd64 only");
    cpy.add(required_option("--prepared", "directory written by `prepare`"));
    cpy.add(required_option("--logs-dir", "logs behind --prepared"));
    add_loader_options(cpy, {"gcc", "clang"}, false);
    cpy.add(value_option("--official-py27",
                         "official fcv1-py27 extension (default: "
                         "prebuilt/pybind/fcv1-py27/simulator-official.so)"));
    cpy.add(value_option("--official-py36",
                         "official fcv1-py36 extension (default: "
                         "prebuilt/pybind/fcv1-py36/simulator-official.so)"));
    cpy.add(value_option("--record-py27",
                         "v2 recording fcv1-py27 extension (default: "
                         "prebuilt/pybind/fcv1-py27/simulator-record-v2.so; needs --trajectory-shots or "
                         "--check-recording)"));
    cpy.add(value_option("--record-py36",
                         "v2 recording fcv1-py36 extension (default: "
                         "prebuilt/pybind/fcv1-py36/simulator-record-v2.so; needs --trajectory-shots or "
                         "--check-recording)"));
    cpy.add(value_option("--trajectory-shots",
                         "newline-separated shot ids to replay with the v2 recording binaries"))
        .metavar = "FILE";
    cpy.add(bounded_int("--workers", 1, cpus, "worker processes / threads (default: logical CPUs)"));
    cpy.add(bounded_int("--max-steps", 1, 60000));
    cpy.add(bounded_float("--timeout", 0, 600.0,
                          "seconds per pybind worker call (batches of up to 50 shots, --check-recording: 25), "
                          "0 = no limit (default: 600)"));
    cpy.add(hidden_option("--worker-command"));
    cpy.add(hidden_option("--pybind-worker-command"));
    cpy.add_group("check of the recording engines",
                  "--check-recording compares the v2 recording binaries with the official ones on every shot "
                  "(no Path A, no --compiler); exit 0: every final board equal, 1: a difference, 2: an error");
    cpy.add(bool_option("--check-recording",
                        "check the v2 recording binaries against the official ones instead of the C API comparison"));
    cpy.add(value_option("--removal-sheet-width",
                         "FCV1Plus play-area removal of the recording engine: none (default: the official binaries "
                         "have none), shot (the width of each prepared shot) or a number of metres"))
        .metavar = "none|shot|METRES";
    cpy.add(optional_int("--frame-stride", 1, "recording frame stride (default: 1)"));
    cpy.add(optional_int("--limit", 0, "check only the first N prepared shots, 0 = all (default: 0)"));
    cpy.add(value_option("--engines", "comma separated: py27, py36 (default: py27,py36)"));
    cpy.use_group("");
    cpy.add(required_option("--output", "report JSON (never overwritten)"));

    Parser& cplayer = cmp.add_command(
        "player", "two player outputs (.bin from `capture player`), or two `run player` result JSON files");
    cplayer.add(positional("left", "LEFT"));
    cplayer.add(positional("right", "RIGHT"));
    cplayer.add(required_option("--output", "comparison JSON (never overwritten)"));

    Parser& render = root->add_command("render", "render capture archives as images");
    render.add_subparsers("render_command_name");
    Parser& rtraj = render.add_command(
        "trajectory",
        "SVG plot per shot of one or two `capture simulator` archives (A, and B overlaid on it), with the "
        "collisions inferred from the stone positions");
    rtraj.add(positional("a", "A"));
    rtraj.add(positional("b", "B")).optional_positional = true;
    rtraj.add(value_option("--shots",
                           "render only these shots: a newline-separated shot-id file (default: every shot of A; "
                           "with B: the shots in both archives)"))
        .metavar = "FILE";
    rtraj.add(value_option("--logs-dir",
                           "also draw the recorded game-log trajectory of each shot (the logs the captures were "
                           "prepared from; shots are found by shot id)"));
    rtraj.add(bounded_float("--contact-margin", 0, 0.005,
                            "metres added to the stone diameter for the contact test (default: 0.005)"));
    rtraj.add(bounded_int("--workers", 1, cpus, "threads that decode and render shots (default: logical CPUs)"));
    rtraj.add(required_option("--output", "new directory for the SVG files and render.json (never overwritten)"));

    Parser& prebuild = root->add_command(
        "prebuild",
        "rebuild the prebuilt dc_worker, loader and player binaries of this host, the WebAssembly dc_worker and "
        "prebuilt/manifest.json (needs Docker; Windows host: windows-amd64 and linux-amd64; x86-64 Linux host: "
        "linux-amd64; aarch64 Linux host: linux-arm64; wasm where the Emscripten image is the recorded one; the "
        "entries of the other platforms are kept)");
    prebuild.add(value_option("--image",
                             "Docker image of the Linux builds, of the architecture of this host (default: "
                             "dc_determinism-cpu:local)"));
    prebuild.add(value_option("--emsdk-image",
                             "Docker image of the WebAssembly dc_worker and players (default: emscripten/emsdk:6.0.9); "
                             "wasm is rebuilt only when its id is the recorded one (or none is recorded), else it is "
                             "carried; a missing image named here is an error"));
    prebuild.add(bool_option("--worker-only",
                             "rebuild only the native dc_worker (Windows host: windows-amd64 and linux-amd64; the players, "
                             "the loaders, the WebAssembly dc_worker and every other manifest member stay)"));
    prebuild.add(bool_option("--wasm-worker-only",
                             "rebuild only the WebAssembly dc_worker (prebuilt/worker/wasm; Windows or Linux host with "
                             "Docker and node on PATH); nothing else changes"));
    prebuild.add(bool_option("--check",
                             "install nothing: compare the recorded toolchain of this host's platforms first (\"toolchain "
                             "differs\" / \"toolchain not recorded\": exit code 2), rebuild what this host builds and print "
                             "one line per artifact and manifest, identical or differs (exit code 0 all identical, 1 "
                             "something differs); with --worker-only the dc_worker binaries, with --wasm-worker-only the "
                             "WebAssembly bundle"));
    prebuild.add(hidden_option("--image-id"));
    prebuild.add(hidden_option("--build-user"));
    Argument& fragment = prebuild.add(hidden_option("--tool-fragment"));
    fragment.append = true;
    return root;
}

}  // namespace cli
}  // namespace dc_determinism
