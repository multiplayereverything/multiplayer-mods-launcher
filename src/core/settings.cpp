#include "settings.h"

#include <algorithm>
#include <ctime>

#include "fs.h"
#include "json.h"
#include "log.h"
#include "strings.h"
#include "win.h"

namespace fl {

const GameStats* UserData::find(const std::string& gameId) const {
    auto it = games.find(gameId);
    return it == games.end() ? nullptr : &it->second;
}

int64_t unixNow() { return static_cast<int64_t>(std::time(nullptr)); }

UserData loadUserData(const std::wstring& path) {
    UserData d;
    std::string text;
    if (!fs::readText(path, text)) return d;
    std::string err;
    json::Value root = json::parse(text, &err);
    if (!err.empty() || !root.isObject()) {
        // Keep the unreadable file for inspection instead of silently overwriting it later.
        logger().warn(L"Settings file is damaged (" + widen(err) + L"); starting fresh: " + path);
        MoveFileExW(path.c_str(), (path + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING);
        return d;
    }
    auto str = [](const json::Value& o, const char* k) {
        const json::Value* v = o.find(k);
        return v && v->isString() ? v->str() : std::string();
    };
    auto num = [](const json::Value& o, const char* k) {
        const json::Value* v = o.find(k);
        return v && v->isNumber() ? v->number() : 0.0;
    };
    d.selectedGame = str(root, "selectedGame");
    d.category = str(root, "category");
    if (const json::Value* m = root.find("minimize"); m && m->isNumber()) d.minimize = static_cast<int>(m->number());
    if (const json::Value* o = root.find("online"); o && o->isBool()) d.online = o->boolean();
    d.defaultPlayer = str(root, "defaultPlayer");
    d.gamesDir = widen(str(root, "gamesDir"));
    d.playerName = widen(str(root, "playerName"));
    if (const json::Value* o = root.find("showOriginals"); o && o->isBool()) d.showOriginals = o->boolean();
    if (const json::Value* o = root.find("autoUpdate"); o && o->isBool()) d.autoUpdate = o->boolean();
    if (const json::Value* o = root.find("sortMode"); o && o->isNumber()) d.sortMode = std::clamp(static_cast<int>(o->number()), 0, 3);
    if (const json::Value* o = root.find("compactCards"); o && o->isBool()) d.compactCards = o->boolean();
    if (const json::Value* o = root.find("showOffline"); o && o->isBool()) d.showOffline = o->boolean();
    if (const json::Value* w = root.find("window"); w && w->isObject()) {
        d.x = static_cast<int>(num(*w, "x"));
        d.y = static_cast<int>(num(*w, "y"));
        d.width = static_cast<int>(num(*w, "width"));
        d.height = static_cast<int>(num(*w, "height"));
        const json::Value* m = w->find("maximized");
        d.maximized = m && m->boolean();
    }
    if (const json::Value* games = root.find("games"); games && games->isObject()) {
        for (size_t i = 0; i < games->size(); ++i) {
            const json::Value& g = games->at(i);
            if (!g.isObject()) continue;
            GameStats s;
            const json::Value* fav = g.find("favorite");
            s.favorite = fav && fav->boolean();
            s.plays = static_cast<int>(num(g, "plays"));
            s.seconds = static_cast<int64_t>(num(g, "seconds"));
            s.lastPlayed = static_cast<int64_t>(num(g, "lastPlayed"));
            s.player = str(g, "player");
            s.gameDir = widen(str(g, "gameDir"));
            if (const json::Value* swf = g.find("swf"); swf && swf->isObject())
                for (size_t j = 0; j < swf->size(); ++j)
                    if (swf->at(j).isString()) s.swf[swf->keyAt(j)] = widen(swf->at(j).str());
            if (const json::Value* st = g.find("settings"); st && st->isObject())
                for (size_t j = 0; j < st->size(); ++j)
                    if (st->at(j).isString()) s.settings[st->keyAt(j)] = st->at(j).str();
            d.games[games->keyAt(i)] = std::move(s);
        }
    }
    return d;
}

bool saveUserData(const UserData& d, const std::wstring& path, std::wstring* error) {
    json::Value root = json::Value::object();
    root.set("selectedGame", d.selectedGame);
    root.set("category", d.category);
    root.set("minimize", d.minimize);
    root.set("online", d.online);
    root.set("defaultPlayer", d.defaultPlayer);
    root.set("gamesDir", narrow(d.gamesDir));
    root.set("playerName", narrow(d.playerName));
    root.set("showOriginals", d.showOriginals);
    root.set("autoUpdate", d.autoUpdate);
    root.set("sortMode", d.sortMode);
    root.set("compactCards", d.compactCards);
    root.set("showOffline", d.showOffline);
    json::Value w = json::Value::object();
    w.set("x", d.x);
    w.set("y", d.y);
    w.set("width", d.width);
    w.set("height", d.height);
    w.set("maximized", d.maximized);
    root.set("window", std::move(w));
    json::Value games = json::Value::object();
    for (const auto& [id, s] : d.games) {
        json::Value g = json::Value::object();
        g.set("favorite", s.favorite);
        g.set("plays", s.plays);
        g.set("seconds", static_cast<int64_t>(s.seconds));
        g.set("lastPlayed", static_cast<int64_t>(s.lastPlayed));
        if (!s.player.empty()) g.set("player", s.player);
        if (!s.gameDir.empty()) g.set("gameDir", narrow(s.gameDir));
        if (!s.swf.empty()) {
            json::Value swf = json::Value::object();
            for (const auto& [action, file] : s.swf) swf.set(action, narrow(file));
            g.set("swf", std::move(swf));
        }
        if (!s.settings.empty()) {
            json::Value st = json::Value::object();
            for (const auto& [key, value] : s.settings) st.set(key, value);
            g.set("settings", std::move(st));
        }
        games.set(id, std::move(g));
    }
    root.set("games", std::move(games));
    std::string text = json::serialize(root);

    unsigned long code = 0;
    if (!fs::ensureDir(fs::parent(path), nullptr, &code)) {
        if (error) *error = win32ErrorMessage(code);
        return false;
    }
    std::wstring tmp = path + L".tmp";
    if (!fs::writeFile(tmp, text.data(), text.size(), &code) ||
        !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!code) code = GetLastError();
        if (error) *error = win32ErrorMessage(code);
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace fl
