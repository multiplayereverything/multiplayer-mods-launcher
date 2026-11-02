// What the Multiplayer Everything website knows about each game: its stage, the
// quality grade, the newest version, how it connects and what stays in sync.
// The site publishes it as one static file, games.json; the launcher shows it
// next to each game so nobody has to keep the same facts in two places.
// Everything here is optional: a missing field or an unreachable site just
// means less is shown.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fl {

struct Config;

struct SiteGame {
    std::string id, modId, kind;           // kind: "flash" | "pc"
    std::wstring name;
    std::string status;                    // available | testing | not-on-server | coming-soon
    std::string quality;                   // A | B | C | D | X, or empty while it is being tested
    std::wstring version, updated, connect, maintainer, engine;
    // What the site shows for the game itself: used for games the launcher doesn't have yet, and to fill
    // in what a launcher game leaves out.
    std::wstring description, players, platform, color, colorEnd, cover, repo;  // repo: "owner/name" on GitHub  // cover: a path on the site, "covers/x.png"
    // A mod made by someone else: "origin": "community". Anything but "official" counts as community, and a
    // community mod is unverified unless the site says "reviewed": true.
    bool community = false, reviewed = false;
    std::wstring author, authorUrl;        // authorUrl: https://github.com/owner/name, nothing else
    int slots = 0;
    float coverFocus = 0.5f;
    // see, story, enemies, world, items -> synced | partly | unknown
    std::map<std::string, std::string> synced;
    // lab, friends, online2, four, launcher, release -> done | partly | none
    std::map<std::string, std::string> progress;
};

// What the launcher accepts from the website (anything else is dropped, see docs/SECURITY_REVIEW.md): a repository
// as "owner/name" of letters, digits, '-', '_' and '.', and a cover as "covers/<name>.png|jpg|webp".
bool isGithubRepo(const std::wstring& repo);
bool isSiteCover(const std::string& path);
bool isGithubRepoUrl(const std::wstring& url);  // exactly https://github.com/<owner>/<name>

// The site's list. Returns false when `json` isn't a games.json.
bool parseSite(std::string_view json, std::vector<SiteGame>& out);

// The entry for a launcher game: its own `site` id, else its id, else the id the mod has on the server.
const SiteGame* findSite(const std::vector<SiteGame>& list, const std::string& siteId, const std::string& gameId,
                         const std::string& modId);

// The last answer kept on disk, so the launcher works offline and asks the site at most once an hour.
struct SiteCache {
    int64_t fetchedAt = 0;  // unix seconds
    std::string raw;        // games.json exactly as downloaded
    bool fresh(int64_t now, int64_t maxAgeSeconds = 3600) const {
        return !raw.empty() && now >= fetchedAt && now - fetchedAt < maxAgeSeconds;
    }
};
bool loadSiteCache(const std::wstring& path, SiteCache& out);
bool saveSiteCache(const std::wstring& path, const SiteCache& cache);

// Folds the website's list into the launcher's own: a game the website lists that the launcher doesn't
// have becomes a "Coming soon" card (its name, description, players, colors and cover come from the site),
// and a launcher game that leaves out its description, player count or cover gets the site's. Covers
// are looked for in `coverDir` (see siteCoverFile); one that isn't there yet shows the initials art.
void mergeSite(Config& cfg, const std::vector<SiteGame>& list, const std::wstring& coverDir);

// Where the cover of site game `id` is kept: <coverDir>\site-<id>.png (or .jpg, .webp).
std::wstring siteCoverFile(const std::wstring& coverDir, const SiteGame& s);

// Downloads the covers the merged games need and don't have yet (pictures only, small, https). `siteUrl` is
// games.json's own address; covers are relative to its folder. Returns how many arrived.
int fetchSiteCovers(const Config& cfg, const std::vector<SiteGame>& list, const std::wstring& siteUrl,
                    const std::wstring& coverDir);

// Downloads games.json (https only) unless the cache is fresh; falls back to the cache when the site doesn't
// answer. `list` is empty only when there is neither. `error` says why the site couldn't be reached.
bool loadSite(const std::wstring& url, const std::wstring& cachePath, std::vector<SiteGame>& list, std::wstring& error,
              bool forceFetch = false);

}  // namespace fl
