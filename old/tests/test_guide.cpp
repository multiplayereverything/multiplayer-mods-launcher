// The guide page's content: defaults for every kind of game, a game's own words, checked links and
// the website's facts. Built without a window.
#include "core/config.h"
#include "core/guide.h"
#include "core/site.h"
#include "test.h"

using namespace fl;

namespace {

const GuideBlock* firstOf(const GuidePage& p, GuideBlock::Kind k) {
    for (const auto& b : p.blocks)
        if (b.kind == k) return &b;
    return nullptr;
}
int countOf(const GuidePage& p, GuideBlock::Kind k) {
    int n = 0;
    for (const auto& b : p.blocks) n += b.kind == k;
    return n;
}
bool hasHeading(const GuidePage& p, const std::wstring& text) {
    for (const auto& b : p.blocks)
        if (b.kind == GuideBlock::Kind::Heading && b.text == text) return true;
    return false;
}
bool anyText(const GuidePage& p, const std::wstring& part) {
    for (const auto& b : p.blocks)
        if (b.text.find(part) != std::wstring::npos || b.detail.find(part) != std::wstring::npos) return true;
    return false;
}

LoadResult guideConfig(const std::string& games) {
    return loadConfig(R"({ "paths": {"games": "${LAUNCHER_DIR}\\games", "players": "${LAUNCHER_DIR}\\players",
        "data": "${LAUNCHER_DIR}\\data", "flashpoint": []}, "players": [ {"id": "r", "paths": ["r.exe"]} ],
        "games": )" + games + " }",
                      L"C:\\x");
}

}  // namespace

TEST(every_kind_of_game_gets_a_guide_without_writing_one) {
    LoadResult r = guideConfig(R"([
      { "id": "flash", "name": "Flash One", "description": "A game.", "online": { "id": "f1", "join": "p" },
        "actions": [ { "id": "p", "label": "Play online", "swf": "a.swf", "hint": "Press F9." } ] },
      { "id": "lobby", "name": "Lobby One", "online": { "id": "l1", "join": "p", "lobby": { "version": "1" } },
        "actions": [ { "id": "p", "swf": "a.swf" } ] },
      { "id": "solo", "name": "Solo", "actions": [ { "id": "p", "swf": "a.swf" } ] },
      { "id": "soon", "name": "Later", "comingSoon": true, "description": "Planned." },
      { "id": "mod", "name": "My Mod", "mod": { "game": {"name": "Big Game", "executable": "G.exe",
        "detection": [{"type": "path", "path": "C:\\x"}]}, "components": [{"id": "m", "source": "a"}],
        "package": {"github": "o/r"}, "launch": {"hint": "Press F5 in the game."} } }
    ])");
    REQUIRE(r.ok());
    const Config& cfg = r.config;
    GuidePage flash = buildGuide(cfg, *cfg.findGame("flash"), nullptr);
    CHECK(hasHeading(flash, L"About") && hasHeading(flash, L"What you need") && hasHeading(flash, L"How to play"));
    CHECK(countOf(flash, GuideBlock::Kind::Step) == 3 && firstOf(flash, GuideBlock::Kind::Step)->number == 1);
    CHECK(firstOf(flash, GuideBlock::Kind::Step)->text.find(L"Play online") != std::wstring::npos);
    GuidePage lobby = buildGuide(cfg, *cfg.findGame("lobby"), nullptr);
    CHECK(anyText(lobby, L"I'm ready"));
    GuidePage solo = buildGuide(cfg, *cfg.findGame("solo"), nullptr);
    CHECK(countOf(solo, GuideBlock::Kind::Step) == 1 && !hasHeading(solo, L"About"));  // no description: no About
    GuidePage soon = buildGuide(cfg, *cfg.findGame("soon"), nullptr);
    CHECK(countOf(soon, GuideBlock::Kind::Step) == 0 && !hasHeading(soon, L"What you need") && !hasHeading(soon, L"Good to know"));
    CHECK(anyText(soon, L"isn't available yet"));
    GuidePage mod = buildGuide(cfg, *cfg.findGame("mod"), nullptr);
    CHECK(firstOf(mod, GuideBlock::Kind::Bullet)->text.find(L"Big Game") != std::wstring::npos);  // ${GAME} is the game
    CHECK(countOf(mod, GuideBlock::Kind::Step) == 4);  // install, launch, the mod's own hint, friends
    CHECK(anyText(mod, L"Press F5 in the game."));
}

TEST(a_games_own_guide_replaces_the_defaults_and_its_links_are_checked) {
    LoadResult r = guideConfig(R"([
      { "id": "g", "name": "G", "actions": [ { "id": "p", "swf": "a.swf" } ],
        "guide": { "needs": ["One thing."], "steps": ["First.", "Second."], "tips": ["A tip."],
                   "keys": [ { "key": "F9", "does": "Opens it." }, { "key": "", "does": "dropped" } ],
                   "links": [ { "label": "Page", "url": "https://example.com/g" }, { "label": "Bad", "url": "http://example.com" },
                              { "label": "A", "url": "https://example.com/a" }, { "label": "B", "url": "https://example.com/b" },
                              { "label": "C", "url": "https://example.com/c" }, { "label": "D", "url": "https://example.com/d" } ] } }
    ])");
    // The bad link, the key without its text and the fifth link are reported; the good ones still load.
    CHECK(!r.ok());
    const GameSpec* g = r.config.findGame("g");
    REQUIRE(g != nullptr);
    CHECK(g->guide.keys.size() == 1 && g->guide.links.size() == kGuideMaxLinks);
    GuidePage page = buildGuide(r.config, *g, nullptr);
    CHECK(countOf(page, GuideBlock::Kind::Step) == 2 && firstOf(page, GuideBlock::Kind::Step)->text == L"First.");
    CHECK(firstOf(page, GuideBlock::Kind::Bullet)->text == L"One thing.");  // its own needs, not the defaults
    CHECK(firstOf(page, GuideBlock::Kind::Key)->text == L"F9" && page.links.size() == kGuideMaxLinks);
    for (const auto& l : page.links) CHECK(l.url.rfind(L"https://", 0) == 0);
}

TEST(the_guide_adds_what_the_website_says_and_a_github_link_once) {
    LoadResult r = guideConfig(R"([
      { "id": "g", "name": "G", "site": "gee", "description": "D.", "actions": [ { "id": "p", "swf": "a.swf" } ],
        "guide": { "links": [ { "label": "Mine", "url": "https://github.com/owner/name" } ] } }
    ])");
    REQUIRE(r.ok());
    std::vector<SiteGame> list;
    REQUIRE(parseSite(R"({ "games": [ { "id": "gee", "name": "G", "status": "testing", "quality": "B", "version": "0.2",
        "updated": "2026-10-27", "connect": "Steam or LAN", "maintainer": "Us", "repo": "owner/name",
        "synced": { "see": "synced", "story": "partly", "items": "unknown" } } ] })",
                      list));
    const GameSpec& g = r.config.games[0];
    GuidePage page = buildGuide(r.config, g, findSite(list, g.siteId, g.id, g.onlineId));
    REQUIRE(hasHeading(page, L"From the website"));
    CHECK(countOf(page, GuideBlock::Kind::Fact) >= 6);  // stage, quality, version, updated, connect, made by, sync
    bool sync = false;
    for (const auto& b : page.blocks)
        if (b.kind == GuideBlock::Kind::Fact && b.text == L"What stays in sync")
            sync = b.detail.find(L"Story (partly synced)") != std::wstring::npos && b.detail.find(L"Items") == std::wstring::npos;
    CHECK(sync);
    CHECK(page.links.size() == 1);  // the guide already links to that repository: no second button for it
    // Without the website's list there is no such section, and the rest of the page is unaffected.
    GuidePage plain = buildGuide(r.config, g, nullptr);
    CHECK(!hasHeading(plain, L"From the website") && hasHeading(plain, L"How to play"));
}

TEST(every_shipped_game_has_a_guide_page_with_steps) {
    std::wstring path = fs::join(fs::parent(fs::parent(fs::parent(fs::parent(fs::selfExePath())))), L"launcher\\launcher.json");
    if (!fs::isFile(path)) path = L"launcher\\launcher.json";
    LoadResult r = loadConfigFile(path);
    REQUIRE(r.ok());
    int withOwn = 0;
    for (const auto& g : r.config.games) {
        GuidePage page = buildGuide(r.config, g, nullptr);
        CHECK(hasHeading(page, L"How to play"));
        if (!g.comingSoon) CHECK(countOf(page, GuideBlock::Kind::Step) >= 1);
        if (!g.guide.steps.empty()) ++withOwn;
        for (const auto& l : page.links) CHECK(l.url.rfind(L"https://", 0) == 0);
    }
    CHECK(withOwn >= 10);  // the PC mods and most Flash games have their own words
}
