// Installs a Flash player the launcher can't find: the newest release asset
// from the player's GitHub repository (or a fixed URL), checked against its
// SHA-256, unpacked into the player's folder. Runs on a worker thread; no UI.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "vars.h"

namespace fl {

struct InstallStatus {
    enum class Stage { Finding, Downloading, Verifying, Unpacking, Done, Failed } stage = Stage::Finding;
    uint64_t done = 0, total = 0;  // bytes, while downloading
};

struct InstallResult {
    bool ok = false;
    std::wstring exe;      // the installed player
    std::wstring version;  // release tag, when known
    std::wstring error;
};

using InstallProgress = std::function<void(const InstallStatus&)>;

// `vars` must hold PLAYERS_DIR and DATA_DIR. `cancel` may be null.
InstallResult installPlayer(const PlayerSpec& player, const Vars& vars, const InstallProgress& progress,
                            const std::atomic<bool>* cancel);

// The release tag of the launcher-installed player is kept in this file inside its folder.
inline constexpr const wchar_t* kPlayerVersionFile = L".version";

struct PlayerUpdate {
    bool checked = false;    // the newest release was looked up
    bool available = false;  // it is newer than the installed copy
    bool updated = false;    // a newer release is now installed
    std::wstring version;    // the newest release's tag
    std::wstring installed;  // the installed copy's tag (empty: not recorded)
    std::wstring error;
};
// When the launcher installed `player` from a GitHub release and a newer release exists, installs it over the old
// one (`install` false: only says whether there is one, so a front end can ask first). A player the launcher didn't
// install (not in install.dir) is left alone. Needs the player not to be running.
PlayerUpdate updateInstalledPlayer(const PlayerSpec& player, const Vars& vars, const std::atomic<bool>* cancel,
                                   bool install = true, const InstallProgress& progress = nullptr);

// One downloadable game file from the game catalog (catalog.json in the
// flash-games repository): `name` is the file name in the games folder and
// `game` the launcher's game id.
struct CatalogFile {
    std::string game;
    std::wstring name, url, sha256, description;
    uint64_t size = 0;
};

// `baseUrl` is the catalog's own address; file paths are relative to its folder.
bool parseCatalog(std::string_view json, const std::wstring& baseUrl, std::vector<CatalogFile>& out,
                  std::wstring& error);
// The catalog's "settings": game id -> that game's settings (see Setting in config.h). A game with no entry, or
// an entry that doesn't parse, keeps the settings from launcher.json.
void parseCatalogSettings(std::string_view json, std::map<std::string, std::vector<Setting>>& out);
// Downloads and parses the catalog. `raw` receives the JSON for caching.
bool fetchCatalog(const std::wstring& url, std::vector<CatalogFile>& out, std::string& raw, std::wstring& error,
                  const std::atomic<bool>* cancel = nullptr);
// Downloads `files` into `dir`, each checked against its SHA-256 and written
// under a temporary name first. Files already there with the right checksum are kept.
InstallResult downloadGameFiles(const std::vector<CatalogFile>& files, const std::wstring& dir,
                                const InstallProgress& progress, const std::atomic<bool>* cancel);
// Catalog files that exist in `dir` with a different SHA-256: newer builds are
// published. Reads every such file, so call it off the UI thread.
std::vector<CatalogFile> outdatedGameFiles(const std::vector<CatalogFile>& catalog, const std::wstring& dir);

// The launcher updating itself.
struct UpdateResult {
    bool ok = false;       // checked (and installed, if there was something newer)
    bool updated = false;  // a newer exe is now in place; it runs from the next start
    std::wstring version;  // the newest release's tag
    std::wstring error;
};
// Downloads the newest release's `source.asset` when it is newer than
// `currentVersion`, checks its SHA-256 (required) and swaps it in for
// `exePath`; the running exe is kept as `exePath`.old until removeOldUpdate.
UpdateResult updateSelf(const InstallSpec& source, const std::wstring& currentVersion, const std::wstring& exePath,
                        const std::atomic<bool>* cancel);
void removeOldUpdate(const std::wstring& exePath);

// What a GitHub "rate limit used up" answer says to the player, given the seconds until it resets.
std::wstring rateLimitMessage(int64_t secondsLeft);
bool isRateLimitMessage(const std::wstring& text);

// A cover picture from the website: https only, at most `maxBytes`, and it must start like a PNG or JPEG,
// otherwise it is deleted again. Written under a temporary name first.
bool downloadImage(const std::wstring& url, const std::wstring& dest, uint64_t maxBytes, std::wstring& error);

// Small text download (JSON); false with `error` set on failure.
bool httpGetText(const std::wstring& url, std::string& out, std::wstring& error);

}  // namespace fl
