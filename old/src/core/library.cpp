#include "library.h"

#include <algorithm>

#include "fs.h"
#include "log.h"
#include "strings.h"
#include "win.h"

namespace fl {

Library::Library(const Config& cfg, Vars base) : cfg_(cfg), vars_(std::move(base)) {
    vars_.set(L"TITLE", cfg.branding.title);
    vars_.set(L"SUBTITLE", cfg.branding.subtitle);
    refresh();
}

bool Library::isUrl(const std::wstring& s) { return startsWithI(s, L"http://") || startsWithI(s, L"https://"); }

namespace {

// Expands one candidate; returns empty when it references a variable that is
// unknown or empty (e.g. ${FLASHPOINT} on a PC without Flashpoint).
std::wstring expandCandidate(const std::wstring& raw, const Vars& vars) {
    std::vector<std::wstring> unknown;
    std::wstring s = vars.expand(raw, &unknown);
    if (!unknown.empty()) return {};
    size_t i = 0;
    while ((i = raw.find(L"${", i)) != std::wstring::npos) {
        size_t end = raw.find(L'}', i);
        if (end == std::wstring::npos) break;
        std::wstring name = raw.substr(i + 2, end - i - 2);
        if (!startsWithI(name, L"env:") && !startsWithI(name, L"folder:") && vars.get(name).empty()) return {};
        i = end + 1;
    }
    return s;
}

bool naturalLess(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && iswdigit(a[i2])) ++i2;
            while (j2 < b.size() && iswdigit(b[j2])) ++j2;
            unsigned long long x = wcstoull(a.substr(i, i2 - i).c_str(), nullptr, 10);
            unsigned long long y = wcstoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
            if (x != y) return x < y;
            i = i2;
            j = j2;
            continue;
        }
        wchar_t x = towlower(a[i]), y = towlower(b[j]);
        if (x != y) return x < y;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::wstring globFile(const std::wstring& pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(fs::longPath(pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::vector<std::wstring> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (names.empty()) return {};
    // Highest name in natural order wins, so "flashplayer_32_sa.exe" beats "flashplayer_9_sa.exe".
    std::sort(names.begin(), names.end(), naturalLess);
    return fs::join(fs::parent(pattern), names.back());
}

std::wstring searchPath(const std::wstring& name) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = SearchPathW(nullptr, name.c_str(), nullptr, static_cast<DWORD>(std::size(buf)), buf, nullptr);
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

}  // namespace

std::wstring Library::findFirst(const std::vector<std::wstring>& candidates, const Vars& vars) const {
    for (const auto& raw : candidates) {
        std::wstring s = expandCandidate(raw, vars);
        if (s.empty()) continue;
        if (isUrl(s)) return s;
        if (s.find(L'\\') == std::wstring::npos && s.find(L'/') == std::wstring::npos) {
            std::wstring found = searchPath(s);
            if (!found.empty()) return fs::normalize(found);
            continue;
        }
        s = fs::normalize(s);
        if (fs::fileName(s).find_first_of(L"*?") != std::wstring::npos) {
            std::wstring found = globFile(s);
            if (!found.empty()) return found;
        } else if (fs::isFile(s)) {
            return s;
        }
    }
    return {};
}

void Library::refresh() {
    vars_.set(L"GAMES_DIR", fs::normalize(gamesOverride_.empty() ? vars_.expand(cfg_.paths.games) : gamesOverride_));
    vars_.set(L"PLAYERS_DIR", fs::normalize(vars_.expand(cfg_.paths.players)));
    vars_.set(L"DATA_DIR", fs::normalize(vars_.expand(cfg_.paths.data)));
    std::wstring flashpoint;
    for (const auto& raw : cfg_.paths.flashpoint) {
        std::wstring p = fs::normalize(vars_.expand(raw));
        if (!p.empty() && fs::isDir(fs::join(p, L"FPSoftware"))) {
            flashpoint = p;
            break;
        }
    }
    vars_.set(L"FLASHPOINT", flashpoint);

    players_.clear();
    for (const auto& p : cfg_.players) {
        PlayerStatus st;
        st.spec = &p;
        st.exe = findFirst(p.paths, vars_);
        logger().info(p.name + L": " + (st.found() ? st.exe : L"not found"));
        players_.push_back(std::move(st));
    }
}

const PlayerStatus* Library::player(const std::string& id) const {
    for (const auto& p : players_)
        if (p.spec->id == id) return &p;
    return nullptr;
}

std::vector<const PlayerSpec*> Library::allowedPlayers(const ActionSpec& a, const std::string& preferred) const {
    std::vector<const PlayerSpec*> out;
    // Flash files go to Flash players, HTML5 pages to browsers.
    const char* kind = a.isWeb() ? "browser" : "flash";
    if (a.players.empty()) {
        for (const auto& p : cfg_.players)
            if (p.kind == kind) out.push_back(&p);
    } else {
        for (const auto& id : a.players)
            if (auto p = cfg_.findPlayer(id); p && p->kind == kind) out.push_back(p);
    }
    auto it = std::find_if(out.begin(), out.end(), [&](const PlayerSpec* p) { return p->id == preferred; });
    if (it != out.end()) std::rotate(out.begin(), it, it + 1);  // the user's choice goes first
    return out;
}

ResolvedAction Library::resolve(const GameSpec& g, const ActionSpec& a, const UserData& user, bool joining) const {
    ResolvedAction r;
    r.game = &g;
    r.action = &a;
    const GameStats* stats = user.find(g.id);
    Vars v = vars_;
    v.set(L"GAME_ID", widen(g.id));
    v.set(L"GAME_NAME", g.name);
    v.set(L"PORT", std::to_wstring(a.relay.port));
    if (a.isLink()) {  // opens a web page: nothing to find
        r.state = ActionState::Ready;
        return r;
    }

    std::wstring userSwf;
    if (stats) {
        auto it = stats->swf.find(a.id);
        if (it != stats->swf.end() && fs::isFile(it->second)) userSwf = it->second;
    }
    std::string preferredPlayer = stats && !stats->player.empty() ? stats->player : user.defaultPlayer;
    if (a.isWeb()) return resolveWeb(r, a, v, userSwf, preferredPlayer);

    bool anyPlayer = false, anySwf = false;
    ResolvedAction fallback;  // best partial match, reported when nothing is ready
    std::string preferred = stats && !stats->player.empty() ? stats->player : user.defaultPlayer;
    for (const PlayerSpec* p : allowedPlayers(a, preferred)) {
        const PlayerStatus* st = player(p->id);
        auto o = a.overrides.find(p->id);
        const PlayerOverride* ov = o == a.overrides.end() ? nullptr : &o->second;
        std::wstring swf = userSwf;
        if (swf.empty()) swf = findFirst(ov && ov->hasSwf ? ov->swf : a.swf, v);
        bool hasExe = st && st->found();
        anyPlayer = anyPlayer || hasExe;
        anySwf = anySwf || !swf.empty();

        ResolvedAction c = r;
        c.player = p;
        c.playerExe = hasExe ? st->exe : std::wstring();
        c.swf = swf;
        c.swfIsUrl = isUrl(swf);
        c.userSwf = !userSwf.empty();
        if (hasExe && !swf.empty()) {
            Vars av = v;
            av.set(L"SWF", swf);
            av.set(L"SWF_DIR", c.swfIsUrl ? fs::parent(c.playerExe) : fs::parent(swf));
            bool mentionsSwf = false;
            auto add = [&](const std::vector<std::wstring>& list) {
                for (const auto& arg : list) {
                    if (arg.find(L"${SWF}") != std::wstring::npos) mentionsSwf = true;
                    c.args.push_back(av.expand(arg));
                }
            };
            add(p->args);
            add(joining && !a.joinArgs.empty() ? a.joinArgs : ov && ov->hasArgs ? ov->args : a.args);
            if (!mentionsSwf) c.args.push_back(swf);
            // Flash loads a game's extra files relative to the working folder.
            c.workDir = av.get(L"SWF_DIR");
            c.state = ActionState::Ready;
            return c;
        }
        // Prefer an installed player; among those (or with none installed), one that has the file.
        if (!fallback.player || (hasExe && fallback.playerExe.empty()) ||
            (!swf.empty() && fallback.swf.empty() && hasExe == !fallback.playerExe.empty()))
            fallback = c;
    }
    // An installed player without its file is a file problem (download or locate
    // it), even when another player's copy "exists", such as a web address for a
    // player that isn't installed. Only with no player at all is it a player problem.
    fallback.state = !anyPlayer && anySwf ? ActionState::NoPlayer : ActionState::NoSwf;
    return fallback;
}

ResolvedAction Library::resolveWeb(ResolvedAction r, const ActionSpec& a, Vars v, const std::wstring& userFile,
                                   const std::string& preferred) const {
    r.web = true;
    // The file the game needs (a server script, a game folder's index.html...).
    std::wstring file = userFile;
    if (file.empty() && !a.swf.empty()) file = findFirst(a.swf, v);
    if (!file.empty()) {
        for (const wchar_t* name : {L"FILE", L"SWF"}) v.set(name, file);
        for (const wchar_t* name : {L"FILE_DIR", L"SWF_DIR"}) v.set(name, fs::parent(file));
    }
    r.swf = file;
    r.userSwf = !userFile.empty();
    if (!a.page.empty()) {
        r.page = findFirst(a.page, v);
        r.pageLocal = !r.page.empty() && !isUrl(r.page);
        if (r.swf.empty()) r.swf = r.page;
        r.swfIsUrl = isUrl(r.page);
    }
    // Browser: the first allowed one that is installed.
    for (const PlayerSpec* p : allowedPlayers(a, preferred)) {
        const PlayerStatus* st = player(p->id);
        if (!r.player || (st && st->found() && r.playerExe.empty())) {
            r.player = p;
            r.playerExe = st && st->found() ? st->exe : std::wstring();
        }
    }
    if ((!a.swf.empty() && file.empty()) || (!a.page.empty() && r.page.empty())) {
        r.state = ActionState::NoSwf;
        return r;
    }
    if (r.playerExe.empty()) {
        r.state = ActionState::NoPlayer;
        return r;
    }
    if (a.server.enabled()) {
        r.serverExe = findFirst(a.server.exe, v);
        if (r.serverExe.empty()) {
            r.state = ActionState::NoRuntime;
            return r;
        }
        for (const auto& arg : a.server.args) r.serverArgs.push_back(v.expand(arg));
        r.serverCwd = fs::normalize(a.server.cwd.empty() ? v.get(L"FILE_DIR") : v.expand(a.server.cwd));
        r.url = v.expand(a.server.url);
    } else if (!r.page.empty() && !r.pageLocal) {
        r.url = r.page;
    }
    // ${URL} stays unexpanded here: a served page's port, a room code or a
    // friend's link are only known when the game starts.
    bool mentionsUrl = false;
    for (const auto& arg : r.player->args) {
        if (arg.find(L"${URL}") != std::wstring::npos) mentionsUrl = true;
        r.args.push_back(v.expand(arg));
    }
    for (const auto& arg : a.args) r.args.push_back(v.expand(arg));
    if (!mentionsUrl) r.args.push_back(L"${URL}");
    r.workDir = r.pageLocal ? fs::parent(r.page) : fs::parent(r.playerExe);
    r.state = ActionState::Ready;
    return r;
}

ActionState Library::gameState(const GameSpec& g, const UserData& user) const {
    ActionState best = ActionState::NoSwf;
    if (g.mod) return ActionState::Ready;  // a PC mod's state is the mod manager's (core/mod)
    for (const auto& a : g.actions) {
        ActionState s = resolve(g, a, user).state;
        if (s == ActionState::Ready) return s;
        if (s == ActionState::NoPlayer || s == ActionState::NoRuntime) best = s;
    }
    return best;
}

}  // namespace fl
