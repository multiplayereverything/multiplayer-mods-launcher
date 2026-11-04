// The shipped config as players get it: launcher.json plus our own games, which left it for mods-info team/ (1.0.42).
// tests/fixtures/team is the published copy (the Rust test the_team_records_in_a_mods_info_checkout_are_the_published_copy
// keeps it equal to mods-info), merged in as the app merges team.json.
#pragma once
#include <string>
#include <vector>

#include "core/community.h"
#include "core/config.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/strings.h"

namespace fl::test {

// The repository's root, from the test exe in old\build\<Config>.
inline std::wstring repoPath(const std::wstring& rel) {
    std::wstring p = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), rel);
    return fs::isFile(p) || fs::isDir(p) ? p : rel;
}

inline std::string readText(const std::wstring& path) {
    std::vector<uint8_t> b;
    fs::readFile(path, b);
    return std::string(b.begin(), b.end());
}

// Since 1.0.43 our Flash games are published in the short "flash" form, which only the app reads; this engine leaves
// them out. Its own Flash code is still tested on the hand-written records they were (tests/fixtures/team-long).
inline std::wstring teamFixture(const std::wstring& id) {
    std::wstring long_ = repoPath(L"tests\\fixtures\\team-long\\" + id + L".json");
    return fs::isFile(long_) ? long_ : repoPath(L"tests\\fixtures\\team\\" + id + L".json");
}

inline void addTeamFixtures(Config& c) {
    json::Value index = json::parse(readText(repoPath(L"tests\\fixtures\\team\\index.json")), nullptr);
    CommunityCache cache;
    if (const json::Value* ids = index.find("mods"))
        for (size_t i = 0; i < ids->size(); ++i) cache.raw.push_back(readText(teamFixture(widen(ids->at(i).str()))));
    mergeTeam(c, cache);
}

inline LoadResult loadShippedFile(const std::wstring& path) {
    LoadResult r = loadConfigFile(path);
    if (r.ok()) addTeamFixtures(r.config);
    return r;
}

}  // namespace fl::test
