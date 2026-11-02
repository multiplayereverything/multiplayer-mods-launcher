#include "guide.h"

#include "strings.h"

namespace fl {

namespace {

using Kind = GuideBlock::Kind;

struct Builder {
    const Config& cfg;
    const GameSpec& g;
    GuidePage page;

    std::wstring gameName() const { return g.mod ? g.mod->game.name : g.name; }
    // ${NAME} and ${GAME} in a game's own text (or a default) mean the mod and the game it goes into.
    std::wstring fill(std::wstring s) const {
        s = replaceAll(std::move(s), L"${NAME}", g.name);
        return replaceAll(std::move(s), L"${GAME}", gameName());
    }
    std::wstring t(const char* key) const { return fill(cfg.t(key)); }

    void heading(const char* key) { page.blocks.push_back({Kind::Heading, 0, cfg.t(key), L""}); }
    void text(const std::wstring& s) {
        if (!s.empty()) page.blocks.push_back({Kind::Text, 0, fill(s), L""});
    }
    void bullet(const std::wstring& s) {
        if (!s.empty()) page.blocks.push_back({Kind::Bullet, 0, fill(s), L""});
    }
    void step(int n, const std::wstring& s) {
        if (!s.empty()) page.blocks.push_back({Kind::Step, n, fill(s), L""});
    }
    void fact(const char* labelKey, const std::wstring& value) {
        if (!value.empty()) page.blocks.push_back({Kind::Fact, 0, cfg.t(labelKey), value});
    }
    bool hasSection() const { return !page.blocks.empty() && page.blocks.back().kind != Kind::Heading; }

    // The action a "Play online" press runs, for its hint.
    const ActionSpec* joinAction() const {
        for (const auto& a : g.actions)
            if (a.id == g.joinAction) return &a;
        return g.actions.empty() ? nullptr : &g.actions.front();
    }
    bool offlineOnly() const {
        if (g.actions.empty()) return false;
        for (const auto& a : g.actions)
            if (!a.offline) return false;
        return true;
    }

    void needs() {
        std::vector<std::wstring> list = g.guide.needs;
        if (list.empty() && !g.comingSoon) {
            if (g.mod) {
                list = {t("guide.need.game"), t("guide.need.version")};
            } else {
                list.push_back(t("guide.need.player"));
                if (!g.onlineId.empty()) list.push_back(t("guide.need.name"));
            }
        }
        if (list.empty()) return;
        heading("guide.h.needs");
        for (const auto& s : list) bullet(s);
    }

    void steps() {
        std::vector<std::wstring> list = g.guide.steps;
        if (list.empty()) {
            if (g.comingSoon) {
                heading("guide.h.steps");
                text(t("guide.soon"));
                return;
            }
            if (g.mod) {
                list = {t("guide.mod.1"), t("guide.mod.launch")};
                if (!g.mod->launch.hint.empty()) list.push_back(g.mod->launch.hint);
                list.push_back(t("guide.mod.friends"));
            } else if (g.lobby.enabled) {
                list = {t("guide.lobby.1"), t("guide.lobby.2"), t("guide.lobby.3")};
            } else if (!g.onlineId.empty()) {
                list = {t("guide.online.1"), t("guide.online.2"), t("guide.online.3")};
            } else if (!g.actions.empty()) {
                list = {t("guide.play.1")};
                if (const ActionSpec* a = joinAction(); a && !a->hint.empty()) list.push_back(a->hint);
            }
        }
        if (list.empty()) return;
        heading("guide.h.steps");
        int n = 0;
        for (const auto& s : list) step(++n, s);
    }

    void keys() {
        if (g.guide.keys.empty()) return;
        heading("guide.h.keys");
        for (const auto& [key, does] : g.guide.keys) page.blocks.push_back({Kind::Key, 0, fill(key), fill(does)});
    }

    void tips() {
        std::vector<std::wstring> list = g.guide.tips;
        if (!g.comingSoon) {
            if (g.mod) list.push_back(t("guide.mod.tip"));
            else if (g.lobby.enabled) list.push_back(t("guide.lobby.tip"));
            else if (!g.onlineId.empty()) list.push_back(t("guide.online.tip"));
        }
        if (list.empty()) return;
        heading("guide.h.tips");
        for (const auto& s : list) bullet(s);
    }

    void website(const SiteGame* s) {
        if (!s) return;
        size_t before = page.blocks.size();
        heading("guide.h.site");
        std::string key = "site.status." + (g.comingSoon ? std::string("coming-soon") : s->status);
        if (cfg.text.count(key)) fact("guide.site.stage", cfg.t(key));
        if (!g.comingSoon && !s->quality.empty() && cfg.text.count("site.quality." + s->quality))
            fact("guide.site.quality", widen(s->quality) + L": " + cfg.t("site.quality." + s->quality));
        if (!g.comingSoon) {
            fact("guide.site.version", s->version);
            fact("guide.site.updated", s->updated);
        }
        fact("guide.site.players", s->players);
        fact("guide.site.connect", s->connect);
        if (s->community) {
            fact("guide.site.origin", cfg.t(s->reviewed ? "guide.site.communityReviewed" : "guide.site.communityUnverified"));
            fact("guide.site.madeBy", s->author.empty() ? s->maintainer : s->author);
        } else {
            fact("guide.site.madeBy", s->maintainer);
        }
        std::wstring sync;
        for (const char* k : {"see", "story", "enemies", "world", "items"}) {
            auto it = s->synced.find(k);
            if (it == s->synced.end() || it->second == "unknown") continue;
            sync += (sync.empty() ? L"" : L", ") + cfg.t(std::string("site.synced.") + k) + L" (" +
                    cfg.t("site.synced." + it->second) + L")";
        }
        fact("guide.site.sync", sync);
        if (page.blocks.size() == before + 1) page.blocks.pop_back();  // nothing under the heading
    }

    void links(const SiteGame* s) {
        for (const auto& l : g.guide.links)
            if (page.links.size() < kGuideMaxLinks) page.links.push_back(l);
        // The mod's own repository, from the website, unless the guide already links there.
        if (s && isGithubRepo(s->repo) && page.links.size() < kGuideMaxLinks) {
            std::wstring url = L"https://github.com/" + s->repo;
            bool have = false;
            for (const auto& l : page.links) have = have || iequals(l.url, url);
            if (!have) page.links.push_back({cfg.t("guide.github"), url});
        }
    }
};

}  // namespace

GuidePage buildGuide(const Config& cfg, const GameSpec& game, const SiteGame* site) {
    Builder b{cfg, game, {}};
    if (!game.description.empty()) {
        b.heading("guide.h.about");
        b.text(game.description);
    }
    b.needs();
    b.steps();
    b.keys();
    b.tips();
    b.website(site);
    b.links(site);
    return std::move(b.page);
}

}  // namespace fl
