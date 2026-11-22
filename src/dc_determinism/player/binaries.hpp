// Player binaries: manifest-verified prebuilt binaries, user-chosen binaries
// and the one place that decides how a player is started.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common/platform.hpp"
#include "json/json.hpp"

namespace dc_determinism {

extern const char* const kPlayerCompareSchema;  // "dc-determinism-player-comparison-v2": compare player on two .bin outputs
extern const char* const kPlayerRevision;       // the player library revision the manifest records

// `binary_key(player, compiler)`: manifest key `wasm/<player>` or `<platform>-<compiler>/<player>`.
std::string player_binary_key(const std::string& player, const std::string& compiler);

// `players_section(root)`: the manifest's `players` section (root empty: the repository root).
Json players_section(const fs::path& root = fs::path());

// `verify_player_binary(key, root)`: the prebuilt binary and its sidecars against the manifest.
// `{key, path, sha256, sidecars, sidecar_paths, compiler, flags, manifest_entry}`.
Json verify_player_binary(const std::string& key, const fs::path& root = fs::path());

// `describe_explicit_player(path, key, root)`: a user-chosen binary; the manifest entry is only
// attached when it matches. `{key, path, sha256, sidecars, compiler, flags, manifest_entry}`.
Json describe_explicit_player(const fs::path& path, const std::string& key, const fs::path& root = fs::path());

// `recheck_binary(info)`: the binary and its sidecars still have the recorded hashes
// ("player binary changed between repetitions").
void recheck_binary(const Json& info);

// `parse_command_hook(text)`: the `--player-command` JSON array of strings.
std::vector<std::string> parse_player_command(const std::string& text);

// `node_version(node)`: `node --version`, null when it cannot be determined.
Json node_version(const std::string& node);

struct PlayerSelection {
    std::vector<std::string> command;  // the command prefix; the output path and sample count follow
    Json binary;                       // the `binary` block of the summary
    Json runtime;                      // the `runtime` block ({} or {"node": ...})
};

// `resolve_player(args)`.
PlayerSelection resolve_player(const std::string& player, const std::string& compiler,
                               const std::string& player_command);

}  // namespace dc_determinism
