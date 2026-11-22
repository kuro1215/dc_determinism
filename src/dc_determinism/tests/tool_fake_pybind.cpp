// A synthetic stand-in for src/worker/pybind/pybind_worker.py used by test_pybind (through the hidden
// `--pybind-worker-command`): it speaks the same JSON protocol (`dc-pybind-request-v1` on stdin,
// `dc-pybind-result-v1` on stdout, `.f32` frame files in `frames_dir`) without Python. Not part of the tool.
//
//   tool_fake_pybind [--shots ID,ID,...] [--log FILE] [--shift N] [--fail ID,...] [--hang ID,...]
//                    [--fail-when-batch ID,...] [--fail-once FILE] [--only-kind official|record]
//                    [--final-delta X] [--bad-frames ID,...] [--garbage] [--wrong-schema] [--drop-row]
//                    --binary SO --sha256 HEX --engine fcv1-py27|fcv1-py36 --kind official|record
//
// The position of a shot is its index in --shots. Official rows carry the fake pybind final board
// (fake_board.hpp), record rows the same final board (plus --final-delta on the first x), steps 100 + position,
// the frames of the synthetic trajectory (stone 1 moved by 0.002 in x from frame --shift on) and the last frame.
// --fail: exit 3 when the request holds one of these shots; --hang: sleep when it does; --fail-when-batch: exit 3
// when the request holds one of these shots and more than one shot; --fail-once: exit 3 on the first call only
// (the file marks that it happened); --bad-frames: write a frame file that is one byte short for these shots;
// --garbage: answer with text that is no JSON; --wrong-schema: answer with another schema; --drop-row: answer one
// row too few; --bad-final: official finals of a shape no board has (one team of one stone). --only-kind restricts all of these misbehaviours to one kind of call.
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common/sha256.hpp"
#include "fake_board.hpp"
#include "json/json.hpp"

namespace {

using dc_determinism::Json;

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> parts;
    std::stringstream stream(text);
    std::string part;
    while (std::getline(stream, part, ',')) {
        if (!part.empty()) parts.push_back(part);
    }
    return parts;
}

bool contains(const std::vector<std::string>& list, const std::string& value) {
    for (const std::string& item : list) {
        if (item == value) return true;
    }
    return false;
}

Json final_json(const fakeboard::Board& board) {
    std::vector<std::array<double, 2>> points = fakeboard::to_final_points(board);
    Json out = Json::array();
    for (std::size_t team = 0; team < 2; ++team) {
        Json list = Json::array();
        for (std::size_t i = 0; i < 8; ++i) list.push_back(Json::array({points[team * 8 + i][0], points[team * 8 + i][1]}));
        out.push_back(list);
    }
    return out;
}

// Appends one line to the call log. The calls of parallel threads run in processes of their own: they take turns (the
// existence of a directory next to the log is the lock), or two of them could overwrite each other's line.
void append_log_line(const std::string& log_file, const std::string& text) {
    std::filesystem::path lock = log_file + ".lock";
    for (int attempt = 0; attempt < 60000; ++attempt) {
        std::error_code ec;
        if (std::filesystem::create_directory(lock, ec)) {
            {
                std::ofstream stream(log_file, std::ios::app | std::ios::binary);
                stream << text << "\n";
            }
            std::filesystem::remove(lock, ec);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> shots, fail, hang, bad_frames, fail_batch;
    std::string log_file, engine, kind, fail_once, only_kind;
    long shift = 0;
    double final_delta = 0.0;
    bool garbage = false, wrong_schema = false, drop_row = false, bad_final = false;
    for (int i = 1; i < argc; ++i) {
        std::string option = argv[i];
        auto value = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (option == "--shots") shots = split(value());
        else if (option == "--log") log_file = value();
        else if (option == "--shift") shift = std::strtol(value().c_str(), nullptr, 10);
        else if (option == "--fail") fail = split(value());
        else if (option == "--hang") hang = split(value());
        else if (option == "--fail-when-batch") fail_batch = split(value());
        else if (option == "--fail-once") fail_once = value();
        else if (option == "--only-kind") only_kind = value();
        else if (option == "--final-delta") final_delta = std::strtod(value().c_str(), nullptr);
        else if (option == "--bad-frames") bad_frames = split(value());
        else if (option == "--garbage") garbage = true;
        else if (option == "--wrong-schema") wrong_schema = true;
        else if (option == "--drop-row") drop_row = true;
        else if (option == "--bad-final") bad_final = true;
        else if (option == "--engine") engine = value();
        else if (option == "--kind") kind = value();
        else if (option == "--binary" || option == "--sha256") value();
    }
    std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    Json request = Json::parse(input);
    if (request["schema"] != "dc-pybind-request-v1") return 4;

    if (!log_file.empty()) {
        Json line = Json::object();
        line["engine"] = engine;
        line["kind"] = kind;
        Json ids = Json::array(), widths = Json::array(), angular = Json::array();
        for (const Json& shot : request["shots"]) {
            ids.push_back(shot["shot_id"]);
            widths.push_back(shot.contains("removal_sheet_width") ? shot["removal_sheet_width"] : Json(nullptr));
            angular.push_back(shot["angular_velocity"]);
        }
        line["ids"] = ids;
        line["widths"] = widths;
        line["angular"] = angular;
        line["frame_stride"] = request["frame_stride"];
        line["request_width"] = request["removal_sheet_width"];
        line["frames_dir"] = request.contains("frames_dir");
        append_log_line(log_file, line.dump());
    }
    bool misbehave = only_kind.empty() || only_kind == kind;
    if (!fail_once.empty() && misbehave) {
        std::ifstream marker(fail_once);
        if (!marker.good()) {
            std::ofstream create(fail_once);
            create << "failed\n";
            std::fprintf(stderr, "simulated first-call failure\n");
            return 3;
        }
    }
    for (const Json& shot : request["shots"]) {
        if (!misbehave) break;
        std::string id = shot["shot_id"].get<std::string>();
        if (contains(hang, id)) std::this_thread::sleep_for(std::chrono::seconds(600));
        if (contains(fail, id) || (request["shots"].size() > 1 && contains(fail_batch, id))) {
            std::fprintf(stderr, "simulated pybind failure\n");
            return 3;
        }
    }
    if (!misbehave) {
        garbage = wrong_schema = drop_row = false;
        bad_frames.clear();
    }
    if (garbage) {
        std::printf("this is not json\n");
        return 0;
    }

    Json rows = Json::array();
    for (const Json& shot : request["shots"]) {
        std::string id = shot["shot_id"].get<std::string>();
        int position = -1;
        for (std::size_t i = 0; i < shots.size(); ++i) {
            if (shots[i] == id) position = static_cast<int>(i);
        }
        if (position < 0) {
            std::fprintf(stderr, "unknown shot %s\n", id.c_str());
            return 5;
        }
        Json row = Json::object();
        row["shot_id"] = id;
        Json final_board = final_json(fakeboard::pybind_board(engine, position));
        if (kind == "official") {
            row["final"] = bad_final ? Json::array({Json::array({Json::array({1.0, 2.0})})}) : final_board;
            rows.push_back(row);
            continue;
        }
        int steps = 100 + position;
        final_board[0][0][0] = final_board[0][0][0].get<double>() + final_delta;
        std::vector<float> frames;
        Json last_frame = Json::array();
        for (int step = 0; step <= steps; ++step) {
            last_frame = Json::array();
            for (int slot = 0; slot < 16; ++slot) {
                fakeboard::State state = fakeboard::trajectory_state(position, step, slot, steps);
                if (state.present && shift > 0 && step >= shift && slot == 1) {
                    state.values[0] = static_cast<double>(static_cast<float>(state.values[0] + 0.002));
                }
                Json state_json = Json::array();
                for (double value : state.values) {
                    frames.push_back(static_cast<float>(value));
                    state_json.push_back(static_cast<double>(static_cast<float>(value)));
                }
                last_frame.push_back(state_json);
            }
        }
        row["final"] = final_board;
        row["steps"] = steps;
        row["removed_steps"] = Json::array();
        row["frame_count"] = steps + 1;
        row["last_frame"] = last_frame;
        if (request.contains("frames_dir")) {
            std::string name = dc_determinism::sha256_hex(id).substr(0, 24) + ".f32";
            std::string bytes;
            for (float value : frames) fakeboard::put_float(bytes, static_cast<double>(value));
            if (contains(bad_frames, id)) bytes.pop_back();
            std::ofstream stream(request["frames_dir"].get<std::string>() + "/" + name, std::ios::binary);
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            row["frames_file"] = name;
        }
        rows.push_back(row);
    }
    if (drop_row && !rows.empty()) rows.erase(rows.size() - 1);
    Json response = Json::object();
    response["schema"] = wrong_schema ? "dc-other-result" : "dc-pybind-result-v1";
    response["rows"] = rows;
    response["python_version"] = "3.99.0-fake";
    response["numpy_version"] = "9.9.9-fake";
    std::printf("%s\n", response.dump().c_str());
    std::fflush(stdout);
    return 0;
}
