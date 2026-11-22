// The shot catalogue: shot sources (folders
// and per-game ZIP files), discovery order, `game_id` / `shot_id`, concede exclusion, sheet width
// from `config.json`, free-guard-zone classification from `game.dcl2`, the duplicate check and
// the canonical shot manifest digest (`input_sha256`).
//
// The catalogue is an in-memory table (rows in discovery order). Errors that Python would raise as ValueError are
// DcDeterminismError.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"
#include "json/pyparse.hpp"

namespace dc_determinism {

class ZipReader;

extern const char* const kShotPattern;   // "shot_e*.json"
extern const char* const kInputDigest;   // "shot-manifest-v1"
constexpr double kFallbackSheetWidth = 4.75;

// A shot file: a plain file (empty archive) or a ZIP member.
struct ShotRef {
    std::string archive;  // UTF-8 path of the ZIP file, empty for a plain file
    std::string member;   // member name, or UTF-8 path of the plain file
    std::string location() const { return archive.empty() ? member : archive + "::" + member; }
};

struct SheetWidth {
    double width = kFallbackSheetWidth;
    std::string source = "fallback";  // "config" | "fallback"
};

// (end, shot) -> "rule_restore" | "none_detected"
using FgzMap = std::map<std::pair<std::int64_t, std::int64_t>, std::string>;

struct ShotRow {
    std::int64_t index = 0;
    ShotRef ref;
    std::string shot_id;
    std::string digest;       // SHA-256 of the shot file
    double width = 0.0;
    std::string width_source;
    std::string fgz;
    // Extra columns kept with every row (game, end_no, shot_no).
    std::string game;
    std::int64_t end = 0;
    std::int64_t shot = 0;
};

// `fgz_classifications(text)`: the first update with a boolean
// free_guard_zone_foul wins for each shot; a malformed line stops the scan. `text` is UTF-8.
FgzMap fgz_classifications(const std::string& text);

// `sheet_width_from_config(config_text)`: float(config["game"]["setting"]["sheet_width"]).
// Throws DcDeterminismError on any failure.
SheetWidth sheet_width_from_config(const std::string& text);

// Reads shot files, configs and game logs in memory; at most one ZIP stays open.
class ShotReader {
public:
    ShotReader();
    ~ShotReader();
    ShotReader(const ShotReader&) = delete;
    ShotReader& operator=(const ShotReader&) = delete;

    void close();
    // Shot members of a ZIP file in sorted order.
    std::vector<std::string> members(const std::string& archive);
    // Bytes of a ZIP member; false when the member does not exist.
    bool optional(const std::string& archive, const std::string& name, std::string& out);
    std::string read(const ShotRef& ref);
    // Sheet width and its source for the game that owns the shot.
    SheetWidth sheet_width(const ShotRef& ref);
    const FgzMap& fgz_classifications(const ShotRef& ref);

private:
    void open(const std::string& path);
    std::pair<std::string, std::string> game_key(const ShotRef& ref) const;

    std::string path_;
    std::unique_ptr<ZipReader> zip_;
    std::unordered_map<std::string, std::size_t> index_;
    bool has_width_key_ = false;
    std::pair<std::string, std::string> width_key_;
    SheetWidth width_;
    bool has_fouls_key_ = false;
    std::pair<std::string, std::string> fouls_key_;
    FgzMap fouls_;
    std::map<std::string, SheetWidth> directory_widths_;  // plain-file games: one decision per directory
};

// `decode_document` / `decode_shot`: the document of a shot file, or its `log` object. `keep`
// (paths start at the document root and must include "log") limits what is stored.
Json decode_shot(const std::string& raw, const std::string& location, const JsonKeepTree* keep = nullptr);

// Game id for logs without game_id: the game directory, or the ZIP path without ".zip".
std::string fallback_game_id(const ShotRef& ref, const fs::path& root);

// Discovery (`discover` / `shot_refs`): calls `visit(ref)` for the shot references of a folder tree
// or a single ZIP file in sorted order, stopping after `limit` references (0: no limit) or when
// `visit` returns false.
void discover_shots(const fs::path& root, std::int64_t limit, ShotReader& reader,
                    const std::function<bool(const ShotRef&)>& visit);

class Catalogue {
public:
    // `root` is the resolved logs directory. Throws DcDeterminismError.
    Catalogue(const fs::path& root, std::int64_t limit);

    std::int64_t limit() const { return limit_; }
    std::size_t count() const { return rows_.size(); }
    std::int64_t skipped() const { return skipped_; }
    const std::string& input_digest() const { return input_digest_; }
    const std::string& input_sha256() const { return input_sha256_; }
    const std::vector<ShotRow>& rows() const { return rows_; }

private:
    std::string manifest_digest() const;

    fs::path root_;
    std::int64_t limit_;
    std::int64_t skipped_ = 0;
    std::vector<ShotRow> rows_;
    std::string input_digest_;
    std::string input_sha256_;
};

struct LoadedShot {
    std::string shot_id;
    std::string source_sha256;
    double sheet_width = 0.0;
    std::string sheet_width_source;
    std::string fgz;
    Json log;
};

// Re-reads cataloged shots by index and rejects changes since catalogue creation. One object per
// thread (it owns a ShotReader); the catalogue is only read.
class ShotSource {
public:
    explicit ShotSource(const Catalogue& catalogue) : catalogue_(&catalogue) {}

    const ShotRow& locate(std::int64_t index) const;
    LoadedShot load(const ShotRow& row, const JsonKeepTree* keep = nullptr);
    void close() { reader_.close(); }

private:
    const Catalogue* catalogue_;
    ShotReader reader_;
};

}  // namespace dc_determinism
