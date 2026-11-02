#include "site.h"

#include "config.h"
#include <algorithm>
#include <cctype>
#include <cstring>

#include "fs.h"
#include "install.h"
#include "json.h"
#include "log.h"
#include "settings.h"
#include "strings.h"

namespace fl {

namespace {

std::string text(const json::Value& o, const char* key) {
    const json::Value* v = o.find(key);
    return v && v->isString() ? v->str() : std::string();
}

bool onlyChars(const std::string& s, const char* extra, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    for (unsigned char c : s)
        if (!(isalnum(c) || strchr(extra, c))) return false;
    return true;
}

std::map<std::string, std::string> stringMap(const json::Value& o, const char* key) {
    std::map<std::string, std::string> out;
    const json::Value* v = o.find(key);
    if (!v || !v->isObject()) return out;
    for (size_t i = 0; i < v->size(); ++i)
        if (v->at(i).isString()) out[v->keyAt(i)] = v->at(i).str();
    return out;
}

}  // namespace

bool isGithubRepo(const std::wstring& repo) {
    size_t slash = repo.find(L'/');
    if (slash == std::wstring::npos || repo.find(L'/', slash + 1) != std::wstring::npos) return false;
    auto part = [](const std::wstring& p) {
        return onlyChars(narrow(p), "-_.", 100) && p != L"." && p != L"..";
    };
    return part(repo.substr(0, slash)) && part(repo.substr(slash + 1));
}

bool isSiteCover(const std::string& path) {
    const std::string dir = "covers/";
    if (path.size() <= dir.size() + 4 || path.compare(0, dir.size(), dir) != 0) return false;
    std::string name = path.substr(dir.size());
    size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext != ".png" && ext != ".jpg" && ext != ".webp") return false;
    return onlyChars(name.substr(0, dot), "-_.", 80) && name.find("..") == std::string::npos;
}

bool isGithubRepoUrl(const std::wstring& url) {
    const std::wstring prefix = L"https://github.com/";
    return url.rfind(prefix, 0) == 0 && isGithubRepo(url.substr(prefix.size()));
}

bool parseSite(std::string_view jsonText, std::vector<SiteGame>& out) {
    out.clear();
    std::string err;
    json::Value root = json::parse(jsonText, &err);
    const json::Value* games = root.isObject() ? root.find("games") : nullptr;
    if (!games || !games->isArray()) return false;
    for (size_t i = 0; i < games->size(); ++i) {
        const json::Value& g = games->at(i);
        if (!g.isObject()) continue;
        SiteGame s;
        s.id = text(g, "id");
        // The id becomes part of a file name (the cover) and of a game id here: letters, digits, '-' and '_' only.
        if (!onlyChars(s.id, "-_", 48)) continue;
        s.modId = text(g, "modId");
        s.kind = text(g, "kind");
        s.name = widen(text(g, "name"));
        s.status = text(g, "status");
        s.quality = text(g, "quality");
        s.version = widen(text(g, "version"));
        s.updated = widen(text(g, "updated"));
        s.connect = widen(text(g, "connect"));
        s.maintainer = widen(text(g, "maintainer"));
        s.engine = widen(text(g, "engine"));
        s.description = widen(text(g, "description"));
        s.players = widen(text(g, "players"));
        s.platform = widen(text(g, "platform"));
        s.color = widen(text(g, "color"));
        s.colorEnd = widen(text(g, "colorEnd"));
        // A picture inside the site's own covers folder, nothing else.
        s.cover = isSiteCover(text(g, "cover")) ? widen(text(g, "cover")) : std::wstring();
        s.repo = isGithubRepo(widen(text(g, "repo"))) ? widen(text(g, "repo")) : std::wstring();
        // Missing origin = official; any value other than "official" is treated as community (the careful reading).
        if (const std::string origin = text(g, "origin"); !origin.empty() && origin != "official") s.community = true;
        s.author = widen(text(g, "author"));
        s.authorUrl = isGithubRepoUrl(widen(text(g, "authorUrl"))) ? widen(text(g, "authorUrl")) : std::wstring();
        if (const json::Value* rv = g.find("reviewed"); rv && rv->isBool()) s.reviewed = rv->boolean();
        if (const json::Value* sl = g.find("slots"); sl && sl->isNumber()) s.slots = static_cast<int>(sl->number());
        if (const json::Value* cf = g.find("coverFocus"); cf && cf->isNumber())
            s.coverFocus = std::clamp(static_cast<float>(cf->number()), 0.0f, 1.0f);
        s.synced = stringMap(g, "synced");
        s.progress = stringMap(g, "progress");
        out.push_back(std::move(s));
    }
    return true;
}

const SiteGame* findSite(const std::vector<SiteGame>& list, const std::string& siteId, const std::string& gameId,
                         const std::string& modId) {
    auto by = [&](const std::string& id, bool mod) -> const SiteGame* {
        if (id.empty()) return nullptr;
        for (const auto& s : list)
            if ((mod ? s.modId : s.id) == id) return &s;
        return nullptr;
    };
    if (const SiteGame* s = by(siteId, false)) return s;
    if (const SiteGame* s = by(gameId, false)) return s;
    return by(modId, true);
}

std::wstring siteCoverFile(const std::wstring& coverDir, const SiteGame& s) {
    std::wstring tail = toLower(s.cover.size() > 5 ? s.cover.substr(s.cover.size() - 5) : s.cover);
    std::wstring ext = tail == L".webp" ? L".webp" : tail.size() >= 4 && tail.substr(tail.size() - 4) == L".jpg" ? L".jpg" : L".png";
    return fs::join(coverDir, L"site-" + widen(s.id) + ext);
}

void mergeSite(Config& cfg, const std::vector<SiteGame>& list, const std::wstring& coverDir) {
    std::vector<bool> used(list.size(), false);
    for (auto& g : cfg.games) {
        const SiteGame* s = findSite(list, g.siteId, g.id, g.onlineId);
        if (!s) continue;
        used[static_cast<size_t>(s - list.data())] = true;
        if (g.description.empty()) g.description = s->description;
        if (g.playerCount.empty()) g.playerCount = s->players;
        if (g.maxPlayers == 0 && s->slots > 0) g.maxPlayers = s->slots;
        if (g.image.empty() && !s->cover.empty()) {
            g.image = siteCoverFile(coverDir, *s);
            g.imageFocus = s->coverFocus;
        }
    }
    for (size_t i = 0; i < list.size(); ++i) {
        const SiteGame& s = list[i];
        if (used[i] || s.name.empty()) continue;
        GameSpec g;
        g.id = "site-" + s.id;
        g.siteId = s.id;
        g.name = s.name;
        g.description = s.description.empty() ? L"A multiplayer mod we are planning." : s.description;
        g.playerCount = s.players;
        g.maxPlayers = s.slots;
        g.comingSoon = true;  // the launcher has no install for it, whatever stage the site says it is in
        g.categories = {s.kind == "pc" ? "pc" : "flash"};
        if (!s.color.empty()) parseColor(s.color, g.color);
        if (!s.colorEnd.empty()) parseColor(s.colorEnd, g.colorEnd);
        if (!s.cover.empty()) {
            g.image = siteCoverFile(coverDir, s);
            g.imageFocus = s.coverFocus;
        }
        cfg.games.push_back(std::move(g));
    }
}

int fetchSiteCovers(const Config& cfg, const std::vector<SiteGame>& list, const std::wstring& siteUrl,
                    const std::wstring& coverDir) {
    size_t slash = siteUrl.rfind(L'/');
    if (slash == std::wstring::npos) return 0;
    std::wstring base = siteUrl.substr(0, slash + 1);
    int got = 0;
    for (const auto& g : cfg.games) {
        const SiteGame* s = findSite(list, g.siteId, g.id, g.onlineId);
        if (!s || s->cover.empty()) continue;
        std::wstring dest = siteCoverFile(coverDir, *s);
        if (g.image != dest || fs::isFile(dest)) continue;  // not using the site's cover, or already have it
        std::wstring err;
        if (downloadImage(base + s->cover, dest, 3 * 1024 * 1024, err)) ++got;
        else logger().info(L"Website cover for " + s->name + L": " + err);
    }
    return got;
}

bool loadSiteCache(const std::wstring& path, SiteCache& out) {
    std::string data, err;
    if (!fs::readText(path, data)) return false;
    json::Value root = json::parse(data, &err);
    if (!root.isObject()) return false;
    const json::Value* at = root.find("at");
    const json::Value* raw = root.find("games");
    if (!at || !at->isNumber() || !raw || !raw->isString()) return false;
    out.fetchedAt = static_cast<int64_t>(at->number());
    out.raw = raw->str();
    return true;
}

bool saveSiteCache(const std::wstring& path, const SiteCache& cache) {
    json::Value root = json::Value::object();
    root.set("at", cache.fetchedAt);
    root.set("games", cache.raw);
    std::string data = json::serialize(root, true);
    fs::ensureDir(fs::parent(path), nullptr, nullptr);
    return fs::writeFile(path, data.data(), data.size(), nullptr);
}

bool loadSite(const std::wstring& url, const std::wstring& cachePath, std::vector<SiteGame>& list, std::wstring& error,
              bool forceFetch) {
    list.clear();
    SiteCache cache;
    bool haveCache = loadSiteCache(cachePath, cache) && parseSite(cache.raw, list);
    int64_t now = unixNow();
    if (haveCache && !forceFetch && cache.fresh(now)) return true;
    if (url.empty() || (!startsWithI(url, L"https://") && !startsWithI(url, L"http://127.0.0.1"))) {
        if (!haveCache) error = L"No address for the website's data";
        return haveCache;
    }
    std::string body;
    std::vector<SiteGame> fetched;
    if (httpGetText(url, body, error) && parseSite(body, fetched)) {
        list = std::move(fetched);
        SiteCache fresh;
        fresh.fetchedAt = now;
        fresh.raw = std::move(body);
        saveSiteCache(cachePath, fresh);
        error.clear();
        return true;
    }
    if (error.empty()) error = L"The website's data isn't in the expected shape";
    if (haveCache) logger().warn(L"Website: " + error + L"; using the copy from " + std::to_wstring((now - cache.fetchedAt) / 60) + L" minutes ago");
    else list.clear();
    return haveCache;
}

}  // namespace fl
