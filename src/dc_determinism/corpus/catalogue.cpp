#include "corpus/catalogue.hpp"

#include <algorithm>
#include <cmath>
#include <system_error>

#include "common/error.hpp"
#include "common/fs.hpp"
#include "common/log.hpp"
#include "common/sha256.hpp"
#include "corpus/pytext.hpp"
#include "json/writer.hpp"
#include "zip/zip.hpp"

namespace dc_determinism {

const char* const kShotPattern = "shot_e*.json";
const char* const kInputDigest = "shot-manifest-v1";

namespace {

// An integer as Python reads it here (ints, bools, integral finite floats).
bool classification_integer(const Json& value, std::int64_t& out) {
    std::string reason;
    switch (value.type()) {
        case Json::value_t::boolean:
        case Json::value_t::number_integer:
        case Json::value_t::number_unsigned:
            return py_int(value, out, reason);
        case Json::value_t::number_float: {
            double number = value.get<double>();
            if (!std::isfinite(number) || number != std::floor(number)) return false;
            return py_int(value, out, reason);
        }
        default:
            return false;
    }
}

std::string posix_dirname(const std::string& name) {
    std::size_t slash = name.rfind('/');
    if (slash == std::string::npos) return std::string();
    std::string head = name.substr(0, slash + 1);
    while (head.size() > 1 && head.back() == '/') head.pop_back();
    return head;
}

std::string posix_basename(const std::string& name) {
    std::size_t slash = name.rfind('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

// Python `Path` ordering of siblings: case-folded on Windows, code point order otherwise.
bool path_name_less(const std::string& a, const std::string& b) {
    if (is_windows()) return ascii_lower(a) < ascii_lower(b);
    return a < b;
}

bool walk(const fs::path& directory, ShotReader& reader, const std::function<bool(const ShotRef&)>& visit) {
    std::vector<std::pair<std::string, fs::path>> entries;
    std::error_code ec;
    for (fs::directory_iterator it(directory, ec), end; !ec && it != end; it.increment(ec)) {
        entries.emplace_back(path_to_utf8(it->path().filename()), it->path());
    }
    if (ec) throw DcDeterminismError("cannot read " + path_to_utf8(directory) + ": " + ec.message());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return path_name_less(a.first, b.first);
    });
    for (const auto& entry : entries) {
        std::error_code status_error;
        fs::file_status status = fs::status(entry.second, status_error);
        if (!status_error && fs::is_directory(status)) {
            fs::file_status link = fs::symlink_status(entry.second, status_error);
            if (!fs::is_symlink(link)) {
                if (!walk(entry.second, reader, visit)) return false;
            }
        } else if (!status_error && fs::is_regular_file(status)) {
            if (is_shot_name(entry.first)) {
                if (!visit(ShotRef{std::string(), path_to_utf8(entry.second)})) return false;
            } else if (is_zip_name(entry.first)) {
                std::string archive = path_to_utf8(entry.second);
                for (const std::string& name : reader.members(archive)) {
                    if (!visit(ShotRef{archive, name})) return false;
                }
            }
        }
    }
    return true;
}

}  // namespace

FgzMap fgz_classifications(const std::string& text) {
    static JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "cmd"});
        tree.add({"log", "state", "end"});
        tree.add({"log", "state", "shot"});
        tree.add({"log", "last_move", "free_guard_zone_foul"});
        return tree;
    }();
    FgzMap found;
    for (const std::string& line : py_splitlines(text)) {
        Json document;
        std::string error;
        if (!py_json_loads(line, document, error, &keep)) break;
        if (!document.is_object() || !document.contains("log")) continue;
        const Json& event = document["log"];
        if (!event.is_object() || !event.contains("cmd") || event["cmd"] != "update") continue;
        if (!event.contains("state") || !event.contains("last_move")) continue;
        const Json& state = event["state"];
        const Json& last = event["last_move"];
        if (!state.is_object() || !last.is_object()) continue;
        std::int64_t end = 0;
        std::int64_t shot = 0;
        if (!state.contains("end") || !state.contains("shot")) continue;
        if (!classification_integer(state["end"], end) || !classification_integer(state["shot"], shot)) continue;
        if (!last.contains("free_guard_zone_foul") || !last["free_guard_zone_foul"].is_boolean()) continue;
        bool foul = last["free_guard_zone_foul"].get<bool>();
        found.emplace(std::make_pair(end, shot - 1), foul ? "rule_restore" : "none_detected");
    }
    return found;
}

SheetWidth sheet_width_from_config(const std::string& text) {
    static JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"game", "setting", "sheet_width"});
        return tree;
    }();
    if (!is_valid_utf8(text)) throw DcDeterminismError("config is not valid UTF-8");
    Json config;
    std::string error;
    if (!py_json_loads(text, config, error, &keep)) throw DcDeterminismError(error);
    if (!config.is_object() || !config.contains("game") || !config["game"].is_object() ||
        !config["game"].contains("setting") || !config["game"]["setting"].is_object() ||
        !config["game"]["setting"].contains("sheet_width")) {
        throw DcDeterminismError("config lacks game.setting.sheet_width");
    }
    SheetWidth out;
    std::string reason;
    if (!py_float(config["game"]["setting"]["sheet_width"], out.width, reason)) throw DcDeterminismError(reason);
    out.source = "config";
    return out;
}

Json decode_shot(const std::string& raw, const std::string& location, const JsonKeepTree* keep) {
    if (!is_valid_utf8(raw)) throw DcDeterminismError("invalid shot log " + location + ": invalid UTF-8");
    Json document;
    std::string error;
    if (!py_json_loads(raw, document, error, keep)) {
        throw DcDeterminismError("invalid shot log " + location + ": " + error);
    }
    if (!document.is_object() || !document.contains("log") || !document["log"].is_object()) {
        throw DcDeterminismError("invalid shot log " + location + ": missing log object");
    }
    return std::move(document["log"]);
}

// ---------------------------------------------------------------------------------- reader

ShotReader::ShotReader() = default;
ShotReader::~ShotReader() = default;

void ShotReader::close() {
    zip_.reset();
    path_.clear();
    index_.clear();
}

void ShotReader::open(const std::string& path) {
    if (zip_ && path_ == path) return;
    close();
    std::unique_ptr<ZipReader> archive;
    try {
        archive.reset(new ZipReader(path_from_utf8(path)));
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError("unreadable ZIP file " + path + ": " + error.what());
    }
    std::unordered_map<std::string, std::size_t> index;
    const std::vector<ZipEntry>& entries = archive->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& entry = entries[i];
        if (!entry.name.empty() && entry.name.back() == '/') continue;
        if (!index.emplace(entry.name, i).second) {
            throw DcDeterminismError("duplicate member " + entry.name + " in ZIP file " + path);
        }
        if (entry.encrypted) throw DcDeterminismError("encrypted member " + entry.name + " in ZIP file " + path);
    }
    path_ = path;
    zip_ = std::move(archive);
    index_ = std::move(index);
}

std::vector<std::string> ShotReader::members(const std::string& archive) {
    open(archive);
    std::vector<std::string> names;
    for (const auto& item : index_) {
        if (is_shot_name(posix_basename(item.first))) names.push_back(item.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool ShotReader::optional(const std::string& archive, const std::string& name, std::string& out) {
    open(archive);
    std::unordered_map<std::string, std::size_t>::const_iterator found = index_.find(name);
    if (found == index_.end()) return false;
    try {
        out = zip_->read(found->second);
    } catch (const DcDeterminismError& error) {
        throw DcDeterminismError("unreadable member " + name + " in ZIP file " + archive + ": " + error.what());
    }
    return true;
}

std::string ShotReader::read(const ShotRef& ref) {
    if (ref.archive.empty()) {
        try {
            return read_file(path_from_utf8(ref.member));
        } catch (const DcDeterminismError& error) {
            throw DcDeterminismError("unreadable shot log " + ref.member + ": " + error.what());
        }
    }
    std::string raw;
    if (!optional(ref.archive, ref.member, raw)) {
        throw DcDeterminismError("missing member " + ref.member + " in ZIP file " + ref.archive);
    }
    return raw;
}

std::pair<std::string, std::string> ShotReader::game_key(const ShotRef& ref) const {
    if (!ref.archive.empty()) return {ref.archive, posix_dirname(ref.member)};
    return {std::string(), path_to_utf8(path_from_utf8(ref.member).parent_path())};
}

SheetWidth ShotReader::sheet_width(const ShotRef& ref) {
    std::pair<std::string, std::string> key = game_key(ref);
    if (has_width_key_ && key == width_key_) return width_;
    SheetWidth width;
    if (!ref.archive.empty()) {
        const std::string& inner = key.second;
        std::string name = inner.empty() ? "config.json" : inner + "/config.json";
        std::string where = key.first + "::" + name;
        std::string raw;
        if (!optional(key.first, name, raw)) {
            log("Warning: config.json not found in " + key.first + "::" + (inner.empty() ? "." : inner) +
                "; using fallback sheet width 4.75.");
        } else {
            try {
                width = sheet_width_from_config(raw);
            } catch (const DcDeterminismError& error) {
                throw DcDeterminismError("invalid config " + where + ": " + error.what());
            }
        }
    } else {
        std::map<std::string, SheetWidth>::const_iterator cached = directory_widths_.find(key.second);
        if (cached != directory_widths_.end()) {
            width = cached->second;
        } else {
            fs::path config = path_from_utf8(key.second) / "config.json";
            std::error_code ec;
            if (!fs::exists(config, ec)) {
                log("Warning: config.json not found in " + key.second + "; using fallback sheet width 4.75.");
            } else {
                try {
                    width = sheet_width_from_config(read_file(config));
                } catch (const DcDeterminismError& error) {
                    throw DcDeterminismError("invalid config " + path_to_utf8(config) + ": " + error.what());
                }
            }
            directory_widths_[key.second] = width;
        }
    }
    has_width_key_ = true;
    width_key_ = key;
    width_ = width;
    return width_;
}

const FgzMap& ShotReader::fgz_classifications(const ShotRef& ref) {
    std::pair<std::string, std::string> key = game_key(ref);
    if (has_fouls_key_ && key == fouls_key_) return fouls_;
    FgzMap fouls;
    std::string raw;
    bool have = false;
    if (!ref.archive.empty()) {
        std::string name = key.second.empty() ? "game.dcl2" : key.second + "/game.dcl2";
        have = optional(key.first, name, raw);
    } else {
        try {
            raw = read_file(path_from_utf8(key.second) / "game.dcl2");
            have = true;
        } catch (const DcDeterminismError&) {
            have = false;
        }
    }
    if (have && is_valid_utf8(raw)) fouls = dc_determinism::fgz_classifications(raw);
    has_fouls_key_ = true;
    fouls_key_ = key;
    fouls_ = std::move(fouls);
    return fouls_;
}

// ---------------------------------------------------------------------------------- discovery

void discover_shots(const fs::path& root, std::int64_t limit, ShotReader& reader,
                    const std::function<bool(const ShotRef&)>& visit) {
    std::int64_t seen = 0;
    std::function<bool(const ShotRef&)> counted = [&](const ShotRef& ref) {
        bool more = visit(ref);
        ++seen;
        return more && !(limit != 0 && seen >= limit);
    };
    std::error_code ec;
    if (fs::is_regular_file(root, ec)) {
        std::string name = path_to_utf8(root.filename());
        if (!is_zip_name(name)) {
            throw DcDeterminismError("logs input must be a directory or a .zip file: " + path_to_utf8(root));
        }
        std::string archive = path_to_utf8(root);
        for (const std::string& member : reader.members(archive)) {
            if (!counted(ShotRef{archive, member})) return;
        }
        return;
    }
    if (!fs::is_directory(root, ec)) throw DcDeterminismError("logs input does not exist: " + path_to_utf8(root));
    if (limit < 0) return;
    walk(root, reader, counted);
}

std::string fallback_game_id(const ShotRef& ref, const fs::path& root) {
    auto relative = [](const fs::path& path, const fs::path& base) {
        fs::path out = path.lexically_relative(base);
        std::string text = out.generic_string();
        if (out.empty() || text == ".." || text.compare(0, 3, "../") == 0) {
            throw DcDeterminismError(path_to_utf8(path) + " is not in the subpath of " + path_to_utf8(base));
        }
        return text;
    };
    if (ref.archive.empty()) {
        return "path:" + relative(path_from_utf8(ref.member).parent_path(), root);
    }
    std::error_code ec;
    fs::path base = fs::is_regular_file(root, ec) ? root.parent_path() : root;
    std::string name = relative(path_from_utf8(ref.archive), base);
    name = name.substr(0, name.size() - 4);
    std::string inner = posix_dirname(ref.member);
    return "path:" + (inner.empty() ? name : name + "/" + inner);
}

// ---------------------------------------------------------------------------------- catalogue

Catalogue::Catalogue(const fs::path& root, std::int64_t limit) : root_(root), limit_(limit) {
    static JsonKeepTree keep = [] {
        JsonKeepTree tree;
        tree.add({"log", "actual_move"});
        tree.add({"log", "game_id"});
        tree.add({"log", "end"});
        tree.add({"log", "shot"});
        return tree;
    }();
    std::vector<ShotRow> found;
    std::unordered_map<std::string, std::size_t> by_id;
    ShotReader reader;
    discover_shots(root_, limit_, reader, [&](const ShotRef& ref) {
        std::string location = ref.location();
        std::string raw = reader.read(ref);
        Json log = decode_shot(raw, location, &keep);
        const Json* move = log.contains("actual_move") ? &log["actual_move"] : nullptr;
        if (move != nullptr && move->is_object() && move->contains("type") && (*move)["type"] == "concede") {
            ++skipped_;
            return true;
        }
        if (move == nullptr || !move->is_object() || !move->contains("velocity") ||
            !(*move)["velocity"].is_object()) {
            throw DcDeterminismError("shot lacks a physical move: " + location);
        }
        std::string game_id;
        if (log.contains("game_id") && log["game_id"].is_string() && !py_is_blank(log["game_id"].get<std::string>())) {
            game_id = log["game_id"].get<std::string>();
        } else {
            game_id = fallback_game_id(ref, root_);
        }
        std::int64_t end = 0;
        std::int64_t shot = 0;
        std::string reason;
        for (const char* key : {"end", "shot"}) {
            if (!log.contains(key)) throw DcDeterminismError("invalid shot " + location + ": '" + key + "'");
            if (!py_int(log[key], key[0] == 'e' ? end : shot, reason)) {
                throw DcDeterminismError("invalid shot " + location + ": " + reason);
            }
        }
        SheetWidth width = reader.sheet_width(ref);
        const FgzMap& fouls = reader.fgz_classifications(ref);
        FgzMap::const_iterator foul = fouls.find({end, shot});
        ShotRow row;
        row.ref = ref;
        row.game = game_id;
        row.end = end;
        row.shot = shot;
        row.shot_id = game_id + "/end:" + std::to_string(end) + "/shot:" + std::to_string(shot);
        row.digest = sha256_hex(raw);
        row.width = width.width;
        row.width_source = width.source;
        row.fgz = foul == fouls.end() ? "unknown" : foul->second;
        std::pair<std::unordered_map<std::string, std::size_t>::iterator, bool> inserted =
            by_id.emplace(row.shot_id, found.size());
        if (!inserted.second) {
            throw DcDeterminismError("duplicate shot ID " + row.shot_id + ": " +
                             found[inserted.first->second].ref.location() + " and " + location);
        }
        found.push_back(std::move(row));
        return true;
    });
    if (found.empty()) throw DcDeterminismError("empty input: no physical shots found");
    std::sort(found.begin(), found.end(), [](const ShotRow& a, const ShotRow& b) {
        int order = a.game.compare(b.game);
        if (order != 0) return order < 0;
        if (a.end != b.end) return a.end < b.end;
        return a.shot < b.shot;
    });
    for (std::size_t i = 0; i < found.size(); ++i) found[i].index = static_cast<std::int64_t>(i);
    rows_ = std::move(found);
    input_digest_ = kInputDigest;
    input_sha256_ = manifest_digest();
}

std::string Catalogue::manifest_digest() const {
    Sha256 hasher;
    hasher.update("[", 1);
    DumpOptions options = DumpOptions::compact(true, false);
    bool first = true;
    for (const ShotRow& row : rows_) {
        Json item = Json::object();
        item["shot_id"] = row.shot_id;
        item["source_sha256"] = row.digest;
        item["sheet_width"] = row.width;
        item["sheet_width_source"] = row.width_source;
        item["fgz"] = row.fgz;
        if (!first) hasher.update(",", 1);
        first = false;
        hasher.update(json_dumps(item, options));
    }
    hasher.update("]", 1);
    return hasher.finish_hex();
}

// ---------------------------------------------------------------------------------- source

const ShotRow& ShotSource::locate(std::int64_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= catalogue_->rows().size()) {
        throw DcDeterminismError("unknown catalogue index: " + std::to_string(index));
    }
    return catalogue_->rows()[static_cast<std::size_t>(index)];
}

LoadedShot ShotSource::load(const ShotRow& row, const JsonKeepTree* keep) {
    std::string raw = reader_.read(row.ref);
    if (sha256_hex(raw) != row.digest) {
        throw DcDeterminismError("shot log changed during evaluation: " + row.ref.location());
    }
    LoadedShot shot;
    shot.log = decode_shot(raw, row.ref.location(), keep);
    shot.shot_id = row.shot_id;
    shot.source_sha256 = row.digest;
    shot.sheet_width = row.width;
    shot.sheet_width_source = row.width_source;
    shot.fgz = row.fgz;
    return shot;
}

}  // namespace dc_determinism
