#include "cli/commands/render.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

#include "capture/archive.hpp"
#include "cli/commands/prepare.hpp"
#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "corpus/catalogue.hpp"
#include "json/pyfloat.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"
#include "logs/log_compare.hpp"
#include "pybind/pybind.hpp"
#include "render/trajectory_svg.hpp"

namespace dc_determinism {
namespace cli {

namespace {

constexpr double kProgressIntervalSeconds = 5.0;
const char* const kColorA = "#00a86b";
const char* const kColorB = "#2563eb";
const char* const kColorLog = "#8f8f8f";
const char* const kContractKeys[] = {"observation", "max_steps", "record_digest", "corpus_input_sha256"};

// One archive of the command.
struct Input {
    std::string tag;
    std::string path;
    std::string color;
    std::unique_ptr<CaptureArchive> archive;
    std::string engine;
    std::optional<std::string> compiler;
    std::string label;
};

Input open_input(const std::string& tag, const std::string& path, const char* color) {
    Input input;
    input.tag = tag;
    input.path = path;
    input.color = color;
    input.archive.reset(new CaptureArchive(path));
    Json engine = json_get(input.archive->contract(), "engine");
    input.engine = engine.is_string() ? engine.get<std::string>() : std::string("unknown");
    Json compiler = json_get(json_get(json_get(input.archive->index(), "provenance"), "loader"), "compiler");
    if (compiler.is_string() && !compiler.get<std::string>().empty()) input.compiler = compiler.get<std::string>();
    input.label = tag + ": " + input.engine + (input.compiler ? " " + *input.compiler : std::string());
    return input;
}

Json describe_input(const Input& input) {
    Json out = Json::object();
    out["path"] = input.path;
    out["sha256"] = sha256_file(path_from_utf8(input.path));
    out["engine"] = input.engine;
    out["compiler"] = input.compiler ? Json(*input.compiler) : Json(nullptr);
    out["label"] = input.label;
    out["shots"] = static_cast<std::int64_t>(input.archive->shot_ids().size());
    return out;
}

// `--output` without trailing separators; its parent must exist.
fs::path normalised_output(const std::string& text) {
    fs::path output = path_from_utf8(text);
    if (output.empty()) throw DcDeterminismError("--output must not be empty");
    while (output.filename().empty() && output.parent_path() != output && !output.parent_path().empty()) {
        output = output.parent_path();
    }
    fs::path parent = output.parent_path();
    if (parent.empty()) parent = fs::path(".");
    std::error_code ec;
    if (!fs::is_directory(parent, ec)) {
        throw DcDeterminismError("the parent directory of the output does not exist: " + path_text(parent));
    }
    return output;
}

// The output directory is created by one exclusive call: it fails when anything exists there.
void create_output_directory(const fs::path& output) {
    std::error_code ec;
    bool created = fs::create_directory(output, ec);
    if (ec) throw DcDeterminismError("cannot create output directory " + path_text(output) + ": " + ec.message());
    if (!created) throw DcDeterminismError("refusing to overwrite existing output: " + path_text(output));
}

struct Shot {
    std::string id;
    std::int64_t index = 0;
};

std::string file_name_of(const Shot& shot) { return std::to_string(shot.index) + ".svg"; }

// The rendered source of one shot of one archive and its `render.json` block.
RenderedSource build_source(const Input& input, const std::string& shot_id, const ZipReader& reader, double margin,
                            Json& block) {
    const Json& row = input.archive->row(shot_id);
    CapturedTrajectory trajectory = input.archive->trajectory(shot_id, reader);
    RenderedSource source;
    source.tag = input.tag;
    source.label = input.label;
    source.color = input.color;
    source.status = row["status"].get<std::string>();
    source.steps = row["steps"].get<std::int64_t>();
    source.data = make_render_source(trajectory);
    source.collisions = infer_collisions(source.data, margin);

    block = Json::object();
    block["status"] = source.status;
    block["steps"] = source.steps;
    block["frames"] = trajectory.frames();
    block["collisions"] = collisions_json(source.tag, source.collisions);
    block["collision_count"] = static_cast<std::int64_t>(source.collisions.size());
    return source;
}

// What the log overlay needs of a shot file: the recorded trajectory.
const JsonKeepTree& trajectory_keep() {
    static const JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "trajectory"});
        return tree;
    }();
    return keep;
}

// The recorded trajectory of a shot as a plotted source (the start board, then the logged frames) and its
// `render.json` block.
RenderedSource build_log_source(ShotSource& source, const ShotRow& row, Json& block) {
    LoadedShot item = source.load(row, &trajectory_keep());
    Trajectory recorded = trajectory_from_log(item.log);
    std::vector<Board> boards;
    boards.reserve(recorded.frames.size() + 1);
    boards.push_back(board_from_json(json_get(json_get(item.log, "trajectory"), "start")));
    for (Board& board : recorded.frames) boards.push_back(std::move(board));

    RenderedSource out;
    out.log = true;
    out.tag = "log";
    out.color = kColorLog;
    out.status = "recorded";
    out.data = make_log_render_source(boards);
    out.label = "log  frames=" + std::to_string(boards.size()) +
                "  seconds_per_frame=" + py_float_repr(recorded.seconds_per_frame);

    block = Json::object();
    block["frames"] = static_cast<std::int64_t>(boards.size());
    block["seconds_per_frame"] = recorded.seconds_per_frame;
    return out;
}

}  // namespace

int command_render_trajectory(const ParsedArgs& args) {
    const std::string path_a = args.text_or("a", "");
    const bool has_b = args.has("b");
    const std::string path_b = args.text_or("b", "");
    double margin = args.real("contact_margin");
    if (margin == 0.0) margin = 0.0;  // a -0.0 becomes +0.0 (it would be written as "-0.0")
    const fs::path output = normalised_output(args.text_or("output", ""));
    refuse_existing(output);

    Input input_a = open_input("A", path_a, kColorA);
    Input input_b;
    if (has_b) input_b = open_input("B", path_b, kColorB);
    const CaptureArchive& archive_a = *input_a.archive;
    const CaptureArchive* archive_b = has_b ? input_b.archive.get() : nullptr;

    // The shots to render: every shot of A (with B: the shots in both), restricted by --shots.
    bool restrict_shots = args.has("shots");
    std::set<std::string> selected;
    if (restrict_shots) {
        for (const std::string& id : read_shot_ids(args.text_or("shots", ""))) {
            if (!archive_a.has(id) && !(archive_b != nullptr && archive_b->has(id))) {
                throw DcDeterminismError("--shots lists a shot that is not in " +
                                         std::string(has_b ? "either archive" : "the archive") + ": " + id);
            }
            selected.insert(id);
        }
    }
    auto wanted = [&](const std::string& id) { return !restrict_shots || selected.count(id) != 0; };

    std::vector<Shot> shots;
    Json a_only = Json::array(), b_only = Json::array();
    std::set<std::int64_t> used_indexes;
    for (const std::string& id : archive_a.shot_ids()) {
        if (!wanted(id)) continue;
        if (archive_b != nullptr && !archive_b->has(id)) {
            a_only.push_back(id);
            continue;
        }
        Json index = archive_a.row(id)["index"];
        if (archive_b != nullptr && archive_b->row(id)["index"] != index) {
            throw DcDeterminismError("shot " + id + " has index " + py_repr(index) + " in A and " +
                                     py_repr(archive_b->row(id)["index"]) + " in B");
        }
        Shot shot;
        shot.id = id;
        shot.index = index.get<std::int64_t>();
        if (!used_indexes.insert(shot.index).second) {
            throw DcDeterminismError("two selected shots have the corpus index " + std::to_string(shot.index));
        }
        shots.push_back(shot);
    }
    if (archive_b != nullptr) {
        for (const std::string& id : archive_b->shot_ids()) {
            if (wanted(id) && !archive_a.has(id)) b_only.push_back(id);
        }
        if (shots.empty()) log("warning: the archives have no shot to render in common");
    }

    Json contract_differences = Json::array();
    if (archive_b != nullptr) {
        for (const char* key : kContractKeys) {
            Json left = json_get(archive_a.contract(), key);
            Json right = json_get(archive_b->contract(), key);
            if (left == right) continue;
            Json difference = Json::object();
            difference["member"] = key;
            difference["a"] = left;
            difference["b"] = right;
            contract_differences.push_back(difference);
            log(std::string("warning: contract ") + key + " differs between A and B (" + py_repr(left) + " vs " +
                py_repr(right) + ")");
        }
    }

    Json inputs = Json::object();
    inputs["a"] = describe_input(input_a);
    inputs["b"] = has_b ? describe_input(input_b) : Json(nullptr);

    // --logs-dir: the recorded trajectories, found by shot id in a catalogue of the logs.
    const bool with_logs = args.has("logs_dir");
    std::unique_ptr<Catalogue> catalogue;
    std::map<std::string, std::size_t> log_rows;  // shot id -> catalogue row
    Json logs_block = nullptr;
    Json log_missing = Json::array();
    if (with_logs) {
        std::string logs_text = args.text_or("logs_dir", "");
        if (logs_text.empty()) throw DcDeterminismError("--logs-dir must not be empty");
        catalogue.reset(new Catalogue(resolve(path_from_utf8(logs_text)), 0));
        for (std::size_t r = 0; r < catalogue->rows().size(); ++r) log_rows.emplace(catalogue->rows()[r].shot_id, r);
        for (const Shot& shot : shots) {
            if (log_rows.count(shot.id) == 0) log_missing.push_back(shot.id);
        }
        Json contract_digest = json_get(archive_a.contract(), "corpus_input_sha256");
        bool matches = contract_digest.is_string() && contract_digest.get<std::string>() == catalogue->input_sha256();
        logs_block = Json::object();
        logs_block["path"] = logs_text;
        logs_block["input_sha256"] = catalogue->input_sha256();
        logs_block["matches_contract_a"] = matches;
        if (!matches) log("warning: the logs are not the corpus behind A (input_sha256 differs); shots are matched by shot id");
        if (!log_missing.empty()) {
            log("warning: " + std::to_string(log_missing.size()) + " shots are not in the logs and are drawn without them");
        }
    }

    create_output_directory(output);
    const std::size_t total = shots.size();
    std::vector<Json> rows(total);
    std::vector<std::string> errors(total);
    std::vector<char> written(total, 0);
    auto remove_written = [&] {
        std::error_code ignored;
        for (std::size_t i = 0; i < total; ++i) {
            if (written[i]) fs::remove(output / path_from_utf8(file_name_of(shots[i])), ignored);
        }
        fs::remove(output, ignored);  // the directory this command created (empty now)
    };

    try {
        const std::int64_t requested = args.integer("workers");
        const std::size_t workers =
            total == 0 ? 0
                       : static_cast<std::size_t>(std::max<std::int64_t>(
                             1, std::min<std::int64_t>(requested, static_cast<std::int64_t>(total))));
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::atomic<std::size_t> first_failed{total};
        std::atomic<std::size_t> running{workers};
        std::mutex open_error_mutex;
        std::string open_error;

        auto work = [&] {
            try {
                std::unique_ptr<ZipReader> reader_a = archive_a.open_reader();
                std::unique_ptr<ZipReader> reader_b;
                if (archive_b != nullptr) reader_b = archive_b->open_reader();
                std::unique_ptr<ShotSource> log_source;
                if (catalogue) log_source.reset(new ShotSource(*catalogue));
                for (;;) {
                    std::size_t i = next.fetch_add(1);
                    if (i >= total || i > first_failed.load()) break;
                    try {
                        const Shot& shot = shots[i];
                        std::vector<RenderedSource> sources;
                        Json block_a, block_b;
                        sources.push_back(build_source(input_a, shot.id, *reader_a, margin, block_a));
                        Json source_blocks = Json::object();
                        source_blocks["a"] = block_a;
                        if (archive_b != nullptr) {
                            sources.push_back(build_source(input_b, shot.id, *reader_b, margin, block_b));
                            source_blocks["b"] = block_b;
                        }
                        Json log_block = nullptr;
                        if (log_source) {
                            auto found = log_rows.find(shot.id);
                            if (found != log_rows.end()) {
                                sources.push_back(build_log_source(*log_source, catalogue->rows()[found->second], log_block));
                            }
                        }
                        write_file_new(output / path_from_utf8(file_name_of(shot)), render_shot_svg(shot.id, sources));
                        written[i] = 1;
                        Json row = Json::object();
                        row["index"] = shot.index;
                        row["shot_id"] = shot.id;
                        row["file"] = file_name_of(shot);
                        row["sources"] = source_blocks;
                        row["log"] = log_block;
                        rows[i] = row;
                    } catch (const std::exception& exc) {
                        errors[i] = exc.what();
                        std::size_t seen = first_failed.load();
                        while (i < seen && !first_failed.compare_exchange_weak(seen, i)) {
                        }
                    }
                    done.fetch_add(1);
                }
            } catch (const std::exception& exc) {
                // An archive could not be opened again.
                // (under a mutex: several workers may fail to open; the first one recorded is reported)
                std::lock_guard<std::mutex> lock(open_error_mutex);
                if (open_error.empty()) open_error = exc.what();
                first_failed.store(0);
            }
            running.fetch_sub(1);
        };
        // Joins the started workers on every exit path (a destroyed joinable std::thread would terminate).
        struct Joiner {
            std::vector<std::thread> threads;
            ~Joiner() {
                for (std::thread& thread : threads) {
                    if (thread.joinable()) thread.join();
                }
            }
        } joiner;
        std::vector<std::thread>& threads = joiner.threads;
        for (std::size_t t = 0; t < workers; ++t) threads.emplace_back(work);
        auto last_report = std::chrono::steady_clock::now();
        while (running.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last_report).count() >=
                kProgressIntervalSeconds) {
                log("rendered shots " + std::to_string(done.load()) + "/" + std::to_string(total));
                last_report = std::chrono::steady_clock::now();
            }
        }
        for (std::thread& thread : threads) thread.join();
        if (!open_error.empty()) throw DcDeterminismError(open_error);
        if (first_failed.load() < total) throw DcDeterminismError(errors[first_failed.load()]);

        Json rows_json = Json::array();
        for (Json& row : rows) rows_json.push_back(std::move(row));
        Json report = Json::object();
        report["schema"] = kRenderSchema;
        report["dc_determinism"] = tool_identity();
        report["inputs"] = inputs;
        report["logs"] = logs_block;
        report["contact_margin_m"] = margin;
        report["contract_differences"] = contract_differences;
        report["shots"] = rows_json;
        report["a_only"] = a_only;
        report["b_only"] = b_only;
        report["log_missing"] = log_missing;
        write_json_new(output / path_from_utf8("render.json"), report);
    } catch (...) {
        remove_written();
        throw;
    }
    log("render trajectory: " + std::to_string(total) + " shots; wrote " + path_text(output));
    return 0;
}

}  // namespace cli
}  // namespace dc_determinism
