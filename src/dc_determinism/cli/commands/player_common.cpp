#include "cli/commands/player_common.hpp"

#include <cstdio>
#include <random>
#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/text.hpp"
#include "corpus/pytext.hpp"
#include "json/pyparse.hpp"
#include "json/writer.hpp"
#include "process/process.hpp"

namespace dc_determinism {
namespace cli {

namespace {

constexpr std::size_t kStderrTail = 2000;
constexpr std::int64_t kBlocks = 256;
void append_code_point(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// `bytes.decode("utf-8", errors="replace")` as UTF-8 text: every maximal invalid subpart becomes
// one U+FFFD.
std::string decode_replace(const std::string& bytes) {
    std::string out;
    std::size_t i = 0;
    const std::size_t n = bytes.size();
    auto byte_at = [&](std::size_t k) { return static_cast<unsigned char>(bytes[k]); };
    while (i < n) {
        unsigned char lead = byte_at(i);
        if (lead < 0x80) {
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }
        std::size_t length = 0;
        unsigned char low = 0x80, high = 0xBF;  // range of the first continuation byte
        std::uint32_t code = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1F;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0F;
            if (lead == 0xE0) low = 0xA0;
            if (lead == 0xED) high = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07;
            if (lead == 0xF0) low = 0x90;
            if (lead == 0xF4) high = 0x8F;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
            continue;
        }
        std::size_t used = 1;
        bool complete = true;
        for (std::size_t k = 1; k < length; ++k) {
            if (i + k >= n) {
                complete = false;
                break;
            }
            unsigned char next = byte_at(i + k);
            unsigned char lo = k == 1 ? low : 0x80, hi = k == 1 ? high : 0xBF;
            if (next < lo || next > hi) {
                complete = false;
                break;
            }
            code = (code << 6) | (next & 0x3F);
            ++used;
        }
        if (complete) {
            append_code_point(out, code);
        } else {
            out += "\xEF\xBF\xBD";
        }
        i += used;
    }
    return out;
}

// What Python makes of the child's stderr (`text=True, errors="replace"`): replacement
// characters, universal newlines, then `.strip()[-2000:]`.
std::string stderr_tail(const std::string& bytes) {
    std::string text = decode_replace(bytes);
    std::string normalised;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            normalised.push_back('\n');
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            normalised.push_back(text[i]);
        }
    }
    text = py_strip(normalised);
    // The last kStderrTail code points.
    std::size_t points = 0;
    for (unsigned char c : text) points += (c & 0xC0) != 0x80 ? 1 : 0;
    if (points <= kStderrTail) return text;
    std::size_t skip = points - kStderrTail;
    std::size_t at = 0;
    while (at < text.size()) {
        if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
            if (skip == 0) break;
            --skip;
        }
        ++at;
    }
    return text.substr(at);
}

// The first line of the metadata the player printed, as JSON; null when there is none or it is not.
Json parse_metadata(const std::string& bytes) {
    if (!is_valid_utf8(bytes)) return nullptr;
    std::vector<std::string> lines = py_splitlines(bytes);
    if (lines.empty()) return nullptr;
    Json value;
    std::string error;
    if (!py_json_loads(lines[0], value, error)) return nullptr;
    return value;
}

}  // namespace

bool given(const ParsedArgs& args, const char* dest) { return args.has(dest) && !args.text_or(dest, "").empty(); }

void check_samples(std::int64_t samples) {
    if (samples < kMinSamples || samples > kMaxSamples) {
        throw DcDeterminismError("--samples must be within " + std::to_string(kMinSamples) + ".." + std::to_string(kMaxSamples));
    }
}

Scratch::Scratch(const fs::path& output) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
    std::error_code ec;
    if (!output.parent_path().empty()) fs::create_directories(output.parent_path(), ec);
    std::random_device device;
    std::mt19937_64 engine((static_cast<std::uint64_t>(device()) << 32) ^ device());
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::string token;
        for (int i = 0; i < 8; ++i) token.push_back(alphabet[engine() % (sizeof(alphabet) - 1)]);
        fs::path candidate = output.parent_path() / path_from_utf8(path_to_utf8(output.filename()) + ".tmp-" + token);
        if (fs::create_directory(candidate, ec)) {
            path_ = candidate;
            return;
        }
    }
    throw DcDeterminismError("cannot create a temporary directory next to " + path_text(output));
}

Scratch::~Scratch() {
    if (path_.empty()) return;
    std::error_code ec;
    fs::remove_all(path_, ec);
    if (ec) log("warning: cannot remove the temporary directory " + path_text(path_) + ": " + ec.message());
}

// `run_player_once`: one fresh process; the player writes `binary` and prints metadata on stdout.
Json run_player_once(const std::vector<std::string>& command, const fs::path& binary, std::int64_t samples,
                     double timeout) {
    StartOptions options;
    options.argv = command;
    options.argv.push_back(path_text(binary));
    options.argv.push_back(std::to_string(samples));
    RunResult result;
    try {
        result = run_capture(options, std::string(), timeout);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError(std::string("cannot start player: ") + error.what());
    }
    if (result.timed_out) throw DcDeterminismError("player run timed out after " + format_g(timeout) + " s");
    if (result.exit_code != 0) {
        std::string tail = stderr_tail(result.err);
        throw DcDeterminismError("player exited with code " + std::to_string(result.exit_code) + ": " +
                         (tail.empty() ? std::string("no error output") : tail));
    }
    return parse_metadata(result.out);
}

// `decode_file`.
PdetRecord decode_file(const std::string& path_text_value) {
    return decode_bytes(read_file(path_from_utf8(path_text_value)), path_text_value);
}

PdetRecord decode_bytes(std::string data, const std::string& label) {
    try {
        return decode_pdet(std::move(data));
    } catch (const PdetError& error) {
        throw DcDeterminismError("invalid player output " + label + ": " + error.what());
    }
}

// `compare_decoded`.
Json compare_decoded(const PdetRecord& left, const PdetRecord& right) {
    try {
        return compare_pdet(left, right);
    } catch (const PdetError& error) {
        throw DcDeterminismError(std::string("cannot compare player outputs: ") + error.what());
    }
}

// `check_output`: a decoded output must belong to `player` and have `samples` per block.
void check_output(const PdetRecord& record, const std::string& label, const std::string& player, std::int64_t samples) {
    std::uint32_t expected = player == "normal_dist" ? 1 : 2;
    if (record.player != expected) throw DcDeterminismError(label + " is a different player's output");
    bool same = record.samples == kBlocks * samples;
    for (const PdetBlock& block : record.blocks) same = same && static_cast<std::int64_t>(block.n) == samples;
    if (!same) {
        throw DcDeterminismError(label + " has a different sample size (expected " + std::to_string(samples) + " per block)");
    }
}

void print_line(const Json& value) {
    std::printf("%s\n", json_dumps(value).c_str());
    std::fflush(stdout);
}

}  // namespace cli
}  // namespace dc_determinism
