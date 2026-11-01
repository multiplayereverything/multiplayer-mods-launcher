#include "cli_json.h"

#include <cstdio>

#include "core/embedded.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/mod/spec.h"
#include "core/strings.h"

namespace fl {

namespace {

json::Value str(const std::wstring& s) { return json::Value(narrow(s)); }

json::Value strings(const std::vector<std::wstring>& list) {
    json::Value a = json::Value::array();
    for (const auto& s : list) a.push(str(s));
    return a;
}

std::string hex(const Color& c) {
    char buf[8];
    snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
    return buf;
}

const char* stateId(ActionState s) {
    switch (s) {
        case ActionState::Ready: return "ready";
        case ActionState::NoPlayer: return "noPlayer";
        case ActionState::NoSwf: return "noFile";
        case ActionState::NoRuntime: return "noRuntime";
    }
    return "noFile";
}

// Where the game's cover is on disk: its own file, or the one built into the exe written out once under the data folder.
std::wstring coverPath(const GameSpec& g, const Config& cfg, const Library& lib) {
    if (g.image.empty()) return {};
    std::wstring rel = lib.vars().expand(g.image);
    std::wstring path = fs::isAbsolute(rel) ? rel : fs::join(cfg.configDir, rel);
    if (fs::isFile(path) || fs::isAbsolute(rel)) return fs::isFile(path) ? path : std::wstring();
    std::string bytes;
    if (!readEmbedded(rel, bytes)) return {};
    std::wstring out = fs::join(fs::join(lib.dataDir(), L"covers\\built-in"), fs::fileName(rel));
    if (!fs::isFile(out) && (!fs::ensureDir(fs::parent(out), nullptr, nullptr) || !fs::writeFile(out, bytes.data(), bytes.size(), nullptr)))
        return {};
    return out;
}

// The arguments a host or join action passes to Ruffle Together say what it is; the window's Host and Join use them.
void coopFlags(const ActionSpec& a, json::Value& o) {
    bool host = false, relay = false, join = !a.joinArgs.empty(), address = false;
    int players = 0;
    for (size_t k = 0; k < a.args.size(); ++k) {
        const std::wstring& s = a.args[k];
        if (s.rfind(L"--coop-host", 0) == 0) host = true;
        if (s == L"--coop-host-relay") relay = true;
        if (s.rfind(L"--coop-join", 0) == 0) join = true;
        if (s == L"--coop-players" && k + 1 < a.args.size()) players = _wtoi(a.args[k + 1].c_str());
        if (s.find(L"${ADDRESS}") != std::wstring::npos) address = true;
    }
    o.set("coopHost", host);
    o.set("coopJoin", join);
    o.set("joinsByCode", !a.joinArgs.empty());  // --join <code> picks its joinArgs
    o.set("hostPlayers", players);
    o.set("viaRelay", relay);
    o.set("asksAddress", address);
}

}  // namespace

std::string libraryJson(const Config& cfg, const Library& lib, const UserData& user) {
    json::Value root = json::Value::object();
    root.set("version", 1);
    root.set("configDir", str(cfg.configDir));
    root.set("dataDir", str(lib.dataDir()));
    root.set("gamesDir", str(lib.vars().get(L"GAMES_DIR")));
    root.set("flashpoint", str(lib.flashpointDir()));

    json::Value branding = json::Value::object();
    branding.set("title", str(cfg.branding.title));
    branding.set("subtitle", str(cfg.branding.subtitle));
    json::Value colors = json::Value::array();
    for (const auto& c : cfg.branding.playerColors) colors.push(hex(c));
    branding.set("playerColors", std::move(colors));
    root.set("branding", std::move(branding));

    json::Value players = json::Value::array();
    for (const auto& p : lib.players()) {
        json::Value o = json::Value::object();
        o.set("id", p.spec->id);
        o.set("name", str(p.spec->name));
        o.set("found", p.found());
        o.set("exe", str(p.exe));
        o.set("installable", p.spec->install.enabled());
        players.push(std::move(o));
    }
    root.set("players", std::move(players));

    json::Value cats = json::Value::array();
    for (const auto& c : cfg.categories) {
        json::Value o = json::Value::object();
        o.set("id", c.id);
        o.set("name", str(c.name));
        o.set("icon", str(c.icon));
        o.set("group", c.group);
        cats.push(std::move(o));
    }
    root.set("categories", std::move(cats));

    json::Value games = json::Value::array();
    for (const auto& g : cfg.games) {
        json::Value o = json::Value::object();
        o.set("id", g.id);
        o.set("name", str(g.mod && !g.mod->game.name.empty() ? g.mod->game.name : g.name));
        o.set("description", str(g.description));
        o.set("players", str(g.playerCount));
        o.set("maxPlayers", g.maxPlayers);
        o.set("color", hex(g.color));
        o.set("colorEnd", hex(g.colorEnd));
        o.set("image", str(coverPath(g, cfg, lib)));
        o.set("imageFocus", static_cast<double>(g.imageFocus));
        o.set("onlineId", g.onlineId);
        o.set("joinAction", g.joinAction);
        o.set("comingSoon", g.comingSoon);
        json::Value gc = json::Value::array();
        for (const auto& c : g.categories) gc.push(c);
        o.set("categories", std::move(gc));
        if (g.mod) {
            json::Value m = json::Value::object();
            m.set("modName", str(g.mod->mod.name));
            m.set("publisher", str(g.mod->mod.publisher));
            m.set("website", str(g.mod->mod.website));
            m.set("gameName", str(g.mod->game.name));
            m.set("pinnedVersion", str(g.mod->package.sha256.empty() ? std::wstring() : g.mod->package.version));
            o.set("mod", std::move(m));
        }
        json::Value guide = json::Value::object();
        guide.set("needs", strings(g.guide.needs));
        guide.set("steps", strings(g.guide.steps));
        guide.set("tips", strings(g.guide.tips));
        json::Value links = json::Value::array();
        for (const auto& l : g.guide.links) {
            json::Value lo = json::Value::object();
            lo.set("label", str(l.label));
            lo.set("url", str(l.url));
            links.push(std::move(lo));
        }
        guide.set("links", std::move(links));
        o.set("guide", std::move(guide));

        json::Value acts = json::Value::array();
        for (const auto& a : g.actions) {
            json::Value ao = json::Value::object();
            ao.set("id", a.id);
            ao.set("label", str(a.label));
            ao.set("description", str(a.description));
            ao.set("icon", str(a.icon));
            ao.set("more", a.more);
            ao.set("offline", a.offline);
            ao.set("original", a.original);
            ao.set("link", str(a.link));
            ao.set("mod", a.isMod() ? json::Value(a.modOp) : json::Value());
            ao.set("relayPort", a.relay.enabled ? a.relay.port : 0);
            coopFlags(a, ao);
            if (!a.isMod() && !a.isLink()) {
                ResolvedAction r = lib.resolve(g, a, user);
                ao.set("state", stateId(r.state));
                ao.set("player", r.player ? str(r.player->name) : json::Value());
                ao.set("playerId", r.player ? json::Value(r.player->id) : json::Value());
                ao.set("file", str(r.swf));
            }
            acts.push(std::move(ao));
        }
        o.set("actions", std::move(acts));
        games.push(std::move(o));
    }
    root.set("games", std::move(games));

    json::Value u = json::Value::object();
    u.set("playerName", str(user.playerName));
    json::Value stats = json::Value::object();
    for (const auto& [id, s] : user.games) {
        json::Value so = json::Value::object();
        so.set("favorite", s.favorite);
        so.set("plays", s.plays);
        so.set("seconds", static_cast<int64_t>(s.seconds));
        so.set("lastPlayed", static_cast<int64_t>(s.lastPlayed));
        stats.set(id, std::move(so));
    }
    u.set("games", std::move(stats));
    root.set("user", std::move(u));

    json::Value text = json::Value::object();
    for (const auto& [k, v] : cfg.text) text.set(k, str(v));
    root.set("text", std::move(text));
    return json::serialize(root, false);
}

std::string modsJson(const std::vector<ModReport>& mods) {
    json::Value list = json::Value::array();
    for (const auto& m : mods) {
        json::Value o = json::Value::object();
        o.set("id", m.id);
        o.set("gameFound", m.gameFound);
        o.set("gameDir", str(m.gameDir));
        o.set("gameSource", str(m.gameSource));
        o.set("installed", m.installed);
        o.set("installedVersion", str(m.installedVersion));
        o.set("running", m.running);
        o.set("problem", str(m.problem));
        o.set("missing", str(m.missing));
        o.set("known", m.known);
        o.set("published", m.published);
        o.set("stale", m.stale);
        o.set("newest", str(m.newest));
        o.set("minVersion", str(m.minVersion));
        o.set("notice", str(m.notice));
        o.set("error", str(m.error));
        list.push(std::move(o));
    }
    return json::serialize(list, false);
}

std::string roomsJson(const std::wstring& server, bool onServer, const std::vector<RoomInfo>& rooms, const std::wstring& error) {
    json::Value root = json::Value::object();
    root.set("server", str(server));
    root.set("onServer", onServer);
    root.set("error", str(error));
    json::Value list = json::Value::array();
    for (const auto& r : rooms) {
        json::Value o = json::Value::object();
        o.set("code", str(r.code));
        o.set("name", str(r.name));
        o.set("players", r.players);
        o.set("max", r.max);
        o.set("locked", r.locked);
        o.set("state", str(r.state));
        o.set("region", str(r.region));
        o.set("hostPing", r.hostPing);
        list.push(std::move(o));
    }
    root.set("rooms", std::move(list));
    return json::serialize(root, false);
}

}  // namespace fl
