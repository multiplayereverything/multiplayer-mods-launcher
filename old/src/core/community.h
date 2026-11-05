// Mods from mods-info: records/index.json lists the ids, records/<id>.json each one's record (docs/COMMUNITY_MODS.md). The
// record's repository picks the rules: a developer's mod the owner approved, pinned and strict, or one of our own. The
// launcher adds those games without an update of its own. The website's games.json still only decorates cards; it can't
// make anything installable.
// The app (src-tauri/src/launcher/community.rs) downloads them, at most once an hour, into records.json in the data
// folder; this exe only reads that copy, so the mod commands know the same mods as the window.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace fl {

struct Config;

struct CommunityCache {
    int64_t fetchedAt = 0;          // unix seconds
    std::vector<std::string> raw;   // each records/<id>.json exactly as downloaded
    bool fresh(int64_t now, int64_t maxAgeSeconds = 3600) const {
        return fetchedAt > 0 && now >= fetchedAt && now - fetchedAt < maxAgeSeconds;
    }
};

bool loadCommunityCache(const std::wstring& path, CommunityCache& out);
bool saveCommunityCache(const std::wstring& path, const CommunityCache& cache);

// Adds every record that passes readCommunityMod to the games; the others are logged and left out. Returns how
// many were added.
int mergeCommunity(Config& cfg, const CommunityCache& cache);
// The same for the team's own mods, through readTeamMod. Before
// mergeCommunity: a developer's record never takes one of the team's ids.
int mergeTeam(Config& cfg, const CommunityCache& cache);
// mods-info records/ (the app's records.json): both kinds in one list, the team's first.
int mergeRecords(Config& cfg, const CommunityCache& cache);

}  // namespace fl
