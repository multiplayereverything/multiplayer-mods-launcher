// The website's games.json: parsing, matching a launcher game to its entry, and the on-disk copy
// that keeps the launcher working offline.
#include "core/config.h"
#include "core/settings.h"
#include "core/site.h"
#include "test.h"

using namespace fl;

namespace {

const char* kSite = R"({
  "games": [
    { "id": "gunmayhem2", "kind": "flash", "modId": "gm2", "name": "Gun Mayhem 2", "status": "available", "quality": "B",
      "version": "1", "updated": "2026-10-24", "connect": "Hosted relay", "maintainer": "Multiplayer Everything",
      "synced": { "see": "synced", "story": "unknown" } },
    { "id": "baldi", "kind": "pc", "modId": "baldi", "name": "Baldi Together", "status": "testing", "quality": "",
      "version": "0.9.1", "synced": { "see": "synced", "story": "partly" },
      "progress": { "lab": "done", "release": "none" }, "somethingNew": [1, 2] },
    { "name": "no id, skipped" },
    "not an object"
  ]
})";

}  // namespace

TEST(site_json_is_parsed_and_unknown_fields_are_ignored) {
    std::vector<SiteGame> list;
    REQUIRE(parseSite(kSite, list));
    REQUIRE(list.size() == 2);  // the entry without an id and the non-object are skipped
    CHECK(list[0].id == "gunmayhem2" && list[0].modId == "gm2" && list[0].kind == "flash");
    CHECK(list[0].status == "available" && list[0].quality == "B" && list[0].version == L"1");
    CHECK(list[0].connect == L"Hosted relay" && list[0].updated == L"2026-10-24");
    CHECK(list[0].synced.at("see") == "synced" && list[0].synced.at("story") == "unknown");
    CHECK(list[1].quality.empty() && list[1].synced.at("story") == "partly");
    CHECK(list[1].progress.at("lab") == "done");
    CHECK(!parseSite("not json", list) && list.empty());
    CHECK(!parseSite("{ \"other\": [] }", list));
}

TEST(a_game_finds_its_site_entry_by_site_id_then_id_then_mod_id) {
    std::vector<SiteGame> list;
    REQUIRE(parseSite(kSite, list));
    const SiteGame* byId = findSite(list, "", "gunmayhem2", "gm2");
    REQUIRE(byId != nullptr);
    CHECK(byId->id == "gunmayhem2");
    const SiteGame* byMod = findSite(list, "", "something-else", "gm2");
    REQUIRE(byMod != nullptr);
    CHECK(byMod->id == "gunmayhem2");
    const SiteGame* bySite = findSite(list, "baldi", "baldi-together", "");
    REQUIRE(bySite != nullptr);
    CHECK(bySite->id == "baldi");
    CHECK(findSite(list, "", "baldi-together", "") == nullptr);  // no guessing from similar names
    CHECK(findSite(list, "", "", "") == nullptr);
}

TEST(the_site_copy_on_disk_round_trips_and_goes_stale_after_an_hour) {
    TempDir dir("site");
    std::wstring file = dir / L"site.json";
    SiteCache none;
    CHECK(!loadSiteCache(file, none));
    SiteCache c;
    c.fetchedAt = 1000000;
    c.raw = kSite;
    REQUIRE(saveSiteCache(file, c));
    SiteCache back;
    REQUIRE(loadSiteCache(file, back));
    CHECK(back.fetchedAt == 1000000 && back.raw == c.raw);
    CHECK(back.fresh(1000000 + 3599) && !back.fresh(1000000 + 3600) && !back.fresh(999999));
    CHECK(!SiteCache{}.fresh(5));
    writeText(file, "{ broken");
    CHECK(!loadSiteCache(file, none));
}

TEST(a_fresh_copy_is_used_without_asking_and_an_old_one_when_the_site_is_unreachable) {
    TempDir dir("site2");
    std::wstring file = dir / L"site.json";
    SiteCache c;
    c.fetchedAt = unixNow();
    c.raw = kSite;
    REQUIRE(saveSiteCache(file, c));
    std::vector<SiteGame> list;
    std::wstring err;
    // Fresh: no request is made (the address would not answer anyway).
    CHECK(loadSite(L"https://invalid.invalid/games.json", file, list, err) && list.size() == 2);
    // An hour old and the site is unreachable: the old copy still serves.
    c.fetchedAt = unixNow() - 7200;
    REQUIRE(saveSiteCache(file, c));
    CHECK(loadSite(L"http://127.0.0.1:1/games.json", file, list, err) && list.size() == 2);
    // No copy and no site: nothing, and a reason.
    TempDir empty("site3");
    CHECK(!loadSite(L"http://127.0.0.1:1/games.json", empty / L"site.json", list, err) && list.empty() && !err.empty());
    // No address at all: only the copy.
    CHECK(loadSite(L"", file, list, err) && list.size() == 2);
}

TEST(the_site_address_and_a_games_site_id_come_from_launcher_json) {
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "downloads": { "site": "https://example.com/games.json" },
        "games": [ { "id": "g", "name": "G", "site": "gee", "actions": [ { "id": "p", "swf": "a.swf" } ] } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    CHECK(r.config.downloads.site == L"https://example.com/games.json" && r.config.games[0].siteId == "gee");
    LoadResult def = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "g", "name": "G", "actions": [ { "id": "p", "swf": "a.swf" } ] } ] })",
                                L"C:\\x");
    REQUIRE(def.ok());
    CHECK(def.config.downloads.site == L"https://www.multiplayereverything.com/games.json");
    LoadResult bad = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "downloads": { "site": "http://example.com/games.json" }, "games": [] })",
                                L"C:\\x");
    CHECK(!bad.ok());
}

TEST(games_the_launcher_lacks_become_coming_soon_and_the_site_fills_in_what_is_missing) {
    const char* json = R"({ "games": [
      { "id": "native", "name": "Native", "kind": "flash", "status": "available", "quality": "B", "players": "1-4 players",
        "slots": 4, "description": "From the site.", "cover": "covers/native.png", "coverFocus": 0.25 },
      { "id": "planned", "name": "Planned Game", "kind": "pc", "status": "testing", "players": "2-4 players", "slots": 4,
        "description": "A mod in the works.", "color": "#112233", "colorEnd": "#445566", "cover": "covers/planned.png" },
      { "id": "bare", "name": "Bare", "kind": "flash", "status": "coming-soon" }
    ] })";
    std::vector<SiteGame> list;
    REQUIRE(parseSite(json, list));
    LoadResult r = loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": [ { "id": "native", "name": "Native", "actions": [ { "id": "p", "swf": "a.swf" } ] },
                   { "id": "own", "name": "Has everything", "description": "Mine.", "players": "2 players", "maxPlayers": 2,
                     "image": "mine.png", "actions": [ { "id": "p", "swf": "b.swf" } ] } ] })",
                              L"C:\\x");
    REQUIRE(r.ok());
    Config cfg = r.config;
    mergeSite(cfg, list, L"covers");
    REQUIRE(cfg.games.size() == 4);  // two of its own, and the two the site lists that it lacks
    // A game that leaves things out gets the site's.
    CHECK(cfg.games[0].description == L"From the site." && cfg.games[0].playerCount == L"1-4 players");
    CHECK(cfg.games[0].maxPlayers == 4 && cfg.games[0].image.find(L"site-native.png") != std::wstring::npos);
    CHECK(cfg.games[0].imageFocus == 0.25f);
    // One that says it all keeps its own words.
    CHECK(cfg.games[1].description == L"Mine." && cfg.games[1].image == L"mine.png" && cfg.games[1].maxPlayers == 2);
    // The rest become "Coming soon" cards, whatever stage the site gives them.
    const GameSpec* planned = cfg.findGame("site-planned");
    REQUIRE(planned != nullptr);
    CHECK(planned->comingSoon && planned->actions.empty() && planned->name == L"Planned Game");
    CHECK(planned->description == L"A mod in the works." && planned->playerCount == L"2-4 players" && planned->maxPlayers == 4);
    CHECK(planned->color.r == 0x11 && planned->colorEnd.b == 0x66);
    CHECK(planned->categories.size() == 1 && planned->categories[0] == "pc");
    CHECK(planned->image.find(L"site-planned.png") != std::wstring::npos);
    const GameSpec* bare = cfg.findGame("site-bare");
    REQUIRE(bare != nullptr);
    CHECK(bare->comingSoon && !bare->description.empty() && bare->image.empty() && bare->categories[0] == "flash");
    // A launcher game's own `site` id claims its entry, so the site's entry is not added again.
    Config again = r.config;
    again.games[0].siteId = "planned";
    mergeSite(again, list, L"covers");
    CHECK(again.findGame("site-planned") == nullptr);
    CHECK(again.findGame("site-native") != nullptr);  // the entry the native game no longer claims is listed instead
}

TEST(the_website_cannot_steer_a_file_name_or_a_link_the_launcher_uses) {
    const char* json = R"({ "games": [
      { "id": "../escape", "name": "Bad id" },
      { "id": "ok-game_1", "name": "Fine", "repo": "owner/name.v2", "cover": "covers/fine.png" },
      { "id": "has space", "name": "Space" },
      { "id": "x", "name": "Bad repo", "repo": "owner/name/../../other" },
      { "id": "y", "name": "Bad repo 2", "repo": "owner/na me?x=1" },
      { "id": "z", "name": "Bad covers", "cover": "../covers/z.png" },
      { "id": "w", "name": "Other covers", "cover": "https://evil.example/w.png" },
      { "id": "v", "name": "Wrong type", "cover": "covers/v.exe" }
    ] })";
    std::vector<SiteGame> list;
    REQUIRE(parseSite(json, list));
    REQUIRE(list.size() == 6);  // the two with a bad id are dropped
    CHECK(list[0].id == "ok-game_1" && list[0].repo == L"owner/name.v2" && list[0].cover == L"covers/fine.png");
    for (size_t i = 1; i < list.size(); ++i) {
        CHECK(list[i].repo.empty() || list[i].id == "ok-game_1");  // a bad repo is never kept
        CHECK(list[i].cover.empty());                              // nor a cover outside covers/<name>.png|jpg
    }
    CHECK(isGithubRepo(L"owner/name") && isGithubRepo(L"a-b_c/d.e") && !isGithubRepo(L"owner") && !isGithubRepo(L"a/b/c") &&
          !isGithubRepo(L"../x") && !isGithubRepo(L"a/..") && !isGithubRepo(L"a/b c") && !isGithubRepo(L""));
    CHECK(isSiteCover("covers/x.png") && isSiteCover("covers/Some-Game_2.JPG") && !isSiteCover("covers/") &&
          !isSiteCover("covers/a/b.png") && !isSiteCover("covers/..png") && !isSiteCover("x/covers/a.png"));
    // The cover file always sits directly in the covers folder.
    for (const auto& s : list) {
        std::wstring file = siteCoverFile(L"covers", s);
        CHECK(file.find(L"..") == std::wstring::npos && file.find(L"/") == std::wstring::npos);
    }
}

TEST(community_mods_are_marked_unverified_and_their_links_stay_https_github) {
    const char* json = R"({ "games": [
      { "id": "a", "name": "Ours" },
      { "id": "b", "name": "Theirs", "origin": "community", "author": "Sam", "authorUrl": "https://github.com/sam/mod" },
      { "id": "c", "name": "Checked", "origin": "community", "reviewed": true, "authorUrl": "http://github.com/sam/mod" },
      { "id": "d", "name": "Odd", "origin": "whatever", "authorUrl": "https://evil.example/sam/mod" },
      { "id": "e", "name": "Explicit", "origin": "official", "reviewed": "yes" }
    ] })";
    std::vector<SiteGame> list;
    REQUIRE(parseSite(json, list));
    REQUIRE(list.size() == 5);
    CHECK(!list[0].community);                                             // no origin: official
    CHECK(list[1].community && !list[1].reviewed && list[1].author == L"Sam" && list[1].authorUrl == L"https://github.com/sam/mod");
    CHECK(list[2].community && list[2].reviewed && list[2].authorUrl.empty());  // http is dropped
    CHECK(list[3].community && !list[3].reviewed && list[3].authorUrl.empty());  // an unknown origin reads as community
    CHECK(!list[4].community && !list[4].reviewed);                        // reviewed must be a real boolean
    CHECK(isGithubRepoUrl(L"https://github.com/a/b") && !isGithubRepoUrl(L"https://github.com/a/b/c") &&
          !isGithubRepoUrl(L"https://github.com.evil/a/b") && !isGithubRepoUrl(L"https://github.com/../x"));
}

TEST(site_covers_accept_png_jpg_and_webp_only) {
    CHECK(isSiteCover("covers/zombotron.webp"));
    CHECK(isSiteCover("covers/Bowman.PNG"));
    CHECK(isSiteCover("covers/raze.jpg"));
    CHECK(!isSiteCover("covers/x.gif"));
    CHECK(!isSiteCover("covers/../x.webp"));
    CHECK(!isSiteCover("other/x.webp"));
}
