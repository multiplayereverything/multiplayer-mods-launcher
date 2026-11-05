// Entry point: loads launcher.json (--config or next to the exe), then opens
// the launcher window or runs one of the command-line modes (--list,
// --launch, --relay, --validate, --snapshot).
#include "app/cli.h"
#include "app/cli_json.h"
#include "app/lobby_cli.h"

#include <map>

#include "core/community.h"
#include "core/config.h"
#include "core/embedded.h"
#include "core/fs.h"
#include "core/install.h"
#include "core/json.h"
#include "core/library.h"
#include "core/log.h"
#include "core/mod/manager.h"
#include "core/relay.h"
#include "core/rooms.h"
#include "core/settings.h"
#include "core/site.h"
#include "core/strings.h"
#include "ui/launcher.h"

#include <commctrl.h>
#include <shellapi.h>

using namespace fl;

namespace {

bool g_console = false;
HANDLE g_stop = nullptr;

void setupConsole() {
    g_console = true;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;  // redirected
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        HANDLE con = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
        SetStdHandle(STD_OUTPUT_HANDLE, con);
        SetStdHandle(STD_ERROR_HANDLE, con);
    }
}

void write(DWORD which, const std::wstring& s) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0, n = 0;
    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s.c_str(), static_cast<DWORD>(s.size()), &n, nullptr);
    } else {
        std::string u = narrow(replaceAll(s, L"\n", L"\r\n"));
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &n, nullptr);
    }
}

void out(const std::wstring& s) { write(STD_OUTPUT_HANDLE, s); }

void fail(const std::wstring& msg) {
    logger().error(msg);
    if (g_console) write(STD_ERROR_HANDLE, msg + L"\n");
    else MessageBoxW(nullptr, msg.c_str(), L"Multiplayer Everything Launcher", MB_OK | MB_ICONERROR);
}

std::wstring fullPath(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(fs::normalize(p).c_str(), 32768, buf, nullptr);
    return n ? fs::normalize(buf) : fs::normalize(p);
}

BOOL WINAPI ctrlHandler(DWORD type) {
    if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) && g_stop) {
        SetEvent(g_stop);
        return TRUE;
    }
    return FALSE;
}

int runRelay(int port) {
    Relay relay;
    std::wstring err;
    if (!relay.start(port, err)) {
        fail(L"Cannot start the relay on port " + std::to_wstring(port) + L": " + err);
        return 1;
    }
    out(L"Relay listening on port " + std::to_wstring(port) + L". Press Ctrl+C to stop.\n");
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    size_t lastPeers = static_cast<size_t>(-1);
    while (WaitForSingleObject(g_stop, 500) == WAIT_TIMEOUT) {
        RelaySnapshot s = relay.snapshot();
        if (s.peers.size() == lastPeers) continue;
        lastPeers = s.peers.size();
        std::wstring line = std::to_wstring(s.peers.size()) + L" connected";
        for (const auto& p : s.peers) line += L"  [" + p.name + (p.host ? L" (host)" : L"") + L" " + p.address + L"]";
        out(line + L"\n");
    }
    relay.stop();
    return 0;
}

const wchar_t* stateWord(ActionState s) {
    switch (s) {
        case ActionState::Ready: return L"ready";
        case ActionState::NoPlayer: return L"no player";
        case ActionState::NoSwf: return L"game file missing";
        case ActionState::NoRuntime: return L"server program missing";
    }
    return L"?";
}

}  // namespace

// --mods, --install-mod, --repair-mod, --uninstall-mod
int runModCommand(const Args& a, const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath) {
    std::wstring modsInfo = cfg.modsInfoBase();
    auto specFor = [&](const GameSpec& g) {
        mod::Spec s = *g.mod;
        s.text = &cfg.text;
        return s;
    };
    auto chosenDir = [&](const GameSpec& g) {
        if (!a.gameDir.empty()) return fullPath(a.gameDir);
        const GameStats* st = user.find(g.id);
        return st ? st->gameDir : std::wstring();
    };
    if (a.mods) {
        std::vector<ModReport> reports;
        for (const auto& g : cfg.games) {
            if (!g.mod) continue;
            mod::Spec s = specFor(g);
            mod::Status st = mod::inspect(s, lib.vars(), chosenDir(g));
            mod::Source src;
            std::wstring err;
            bool known = mod::resolveSource(s, modsInfo, src, err, cfg.updates.source.api, fs::join(lib.dataDir(), L"mods"));
            if (a.json) {
                ModReport r;
                r.id = g.id;
                r.gameFound = st.game.found();
                r.gameDir = st.game.dir;
                r.gameSource = st.game.source;
                r.installed = st.installed;
                r.installedVersion = st.installedVersion;
                r.running = st.running;
                r.problem = st.problem;
                r.missing = st.missing;
                r.known = known;
                r.published = known && src.published();
                r.stale = known && src.stale;
                r.newest = src.version;
                r.minVersion = src.minVersion;
                r.notice = src.notice;
                r.error = known ? std::wstring() : err;
                reports.push_back(std::move(r));
                continue;
            }
            out(widen(g.id) + L"  " + g.name + L"\n");
            out(L"    game: " + (st.game.found() ? st.game.dir + L" (" + st.game.source + L")" : std::wstring(L"not found")) + L"\n");
            out(L"    installed: " + (st.installed ? st.installedVersion : std::wstring(L"no")) +
                (st.problem.empty() ? L"" : L"  (" + st.problem + L")") + L"\n");
            if (!st.missing.empty()) out(L"    needs: " + st.missing + L"\n");
            out(L"    newest: " + (!known ? L"unknown (" + err + L")" : src.published() ? src.version : std::wstring(L"not published yet")) +
                (known && src.stale ? L"  (couldn't ask GitHub just now: this is the last answer)" : L"") + L"\n");
        }
        if (a.json) out(widen(modsJson(reports)) + L"\n");
        return 0;
    }
    const std::string& id = !a.installMod.empty() ? a.installMod : !a.repairMod.empty() ? a.repairMod : a.uninstallMod;
    const GameSpec* g = cfg.findGame(id);
    if (!g || !g->mod) {
        fail(L"No PC mod with id \"" + widen(id) + L"\" (see --mods)");
        return 3;
    }
    mod::Spec s = specFor(*g);
    mod::Status st = mod::inspect(s, lib.vars(), chosenDir(*g));
    if (!st.game.found()) {
        fail(s.t("mod.status.noGame", mod::withSpec(lib.vars(), s)) + L" Use --game-dir <folder> to point at it.");
        return 4;
    }
    mod::RunRequest req;
    req.gameDir = st.game.dir;
    req.cacheDir = fs::join(lib.dataDir(), L"mods");
    req.githubApi = cfg.updates.source.api;
    if (!a.uninstallMod.empty()) {
        req.op = mod::Operation::Uninstall;
    } else {
        if (st.running) {
            fail(s.t("mod.status.running", mod::withSpec(lib.vars(), s)));
            return 4;
        }
        std::wstring err;
        if (!mod::resolveSource(s, modsInfo, req.source, err, req.githubApi, req.cacheDir)) {
            fail(L"Cannot read mods-info: " + err);
            return 1;
        }
        req.op = !a.repairMod.empty() ? mod::Operation::Repair : st.installed ? mod::Operation::Update : mod::Operation::Install;
        if (req.op == mod::Operation::Repair && !st.installed) {
            fail(L"Nothing to repair: the mod isn't installed.");
            return 4;
        }
    }
    if (!st.problem.empty()) {
        fail(st.problem);
        return 1;
    }
    mod::CancelToken cancel;
    std::wstring lastPhase;
    int lastPct = -1;
    mod::Outcome o = mod::run(s, lib.vars(), req, [&](const std::wstring& phase, double f, const std::wstring&) {
        int pct = static_cast<int>(f * 100);
        if (phase != lastPhase || pct / 25 != lastPct / 25) out(phase + L" " + std::to_wstring(pct) + L"%\n");
        lastPhase = phase;
        lastPct = pct;
    }, cancel);
    if (o.result != mod::Outcome::Result::Success) {
        fail(o.message + (o.detail.empty() ? L"" : L"\n" + o.detail));
        return 1;
    }
    GameStats& gs = user.stats(g->id);
    if (!a.gameDir.empty() && req.op != mod::Operation::Uninstall) gs.gameDir = st.game.dir;
    saveUserData(user, userPath);
    out(req.op == mod::Operation::Uninstall ? L"Uninstalled " + g->name + L".\n"
                                            : L"Done: " + g->name + L" " + req.source.version + L" (" + st.game.dir + L").\n");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Args a = parseArgs(argc, argv);
    LocalFree(argv);
    bool cliMode = a.help || a.version || a.list || a.validate || !a.launch.empty() || a.relayPort || !a.installPlayer.empty() ||
                   !a.downloadGame.empty() || !a.rooms.empty() || a.update || a.updatePlayers || a.mods || !a.installMod.empty() ||
                   !a.repairMod.empty() || !a.uninstallMod.empty() || !a.lobbySession.empty() || !a.error.empty();
    if (cliMode) setupConsole();
    if (!a.error.empty()) {
        fail(a.error + L"\n\n" + usageText());
        return 3;
    }
    if (a.help) {
        out(usageText());
        return 0;
    }
    if (a.version) {
        out(std::wstring(L"Multiplayer Everything Launcher ") + LAUNCHER_VERSION + L"\n");
        return 0;
    }

    std::wstring exeDir = fs::parent(fs::selfExePath());
    std::wstring configPath = a.config.empty() ? fs::join(exeDir, L"launcher.json") : fullPath(a.config);
    if (a.relayPort && !fs::isFile(configPath)) return runRelay(a.relayPort);  // a bare relay needs no config

    // No launcher.json next to the exe: use the one built into it.
    bool builtIn = false;
    std::string embeddedConfig;
    if (a.config.empty() && !fs::isFile(configPath) && readEmbedded(L"launcher.json", embeddedConfig)) builtIn = true;
    LoadResult lr = builtIn ? loadConfig(embeddedConfig, exeDir) : loadConfigFile(configPath);
    if (a.validate) {
        out(formatIssues(lr.issues));
        out(lr.ok() ? L"OK\n" : L"Configuration has errors.\n");
        return lr.ok() ? 0 : 3;
    }
    if (!lr.ok()) {
        fail(L"launcher.json has problems:\n\n" + formatIssues(lr.issues) + L"\n" + configPath);
        return 3;
    }
    const Config& cfg = lr.config;
    Vars base;
    base.set(L"LAUNCHER_DIR", cfg.configDir);
    std::wstring dataDir = fs::normalize(base.expand(cfg.paths.data));
    logger().open(a.log.empty() ? fs::join(dataDir, L"launcher.log") : fullPath(a.log));
    logger().setEcho(!a.launch.empty() || a.relayPort);  // --list and --validate print only their own output
    logger().info(L"Config: " + (builtIn ? std::wstring(L"built into the exe") : configPath));
    for (const auto& i : lr.issues) logger().warn(i.where + L": " + i.message);

    if (a.relayPort) return runRelay(a.relayPort);

    // The mods the app downloaded from mods-info (records.json in the data folder), each checked again here, so --mods,
    // --install-mod and --launch know the same ones as the window. FL_NO_COMMUNITY leaves them out.
    CommunityCache records;
    if (!getenv("FL_NO_COMMUNITY") && loadCommunityCache(fs::join(dataDir, L"records.json"), records)) {
        int added = mergeRecords(lr.config, records);
        if (added) logger().info(L"Mods from mods-info: " + std::to_wstring(added));
    }

    Library lib(cfg, base);
    std::wstring userPath = fs::join(lib.dataDir(), L"settings.json");
    UserData user = loadUserData(userPath);
    if (!user.gamesDir.empty()) {
        lib.setGamesDirOverride(user.gamesDir);
        lib.refresh();
    }
    // The game and player folders belong to the user (Documents by default), so they exist
    // from the first start and are easy to find, not only after the first download.
    fs::ensureDir(lib.vars().get(L"GAMES_DIR"), nullptr, nullptr);
    fs::ensureDir(lib.vars().get(L"PLAYERS_DIR"), nullptr, nullptr);

    if (a.rooms == "all" && a.json) {
        // Every online game's rooms in one call, as the window keeps its cards' room counts: each distinct online
        // id is asked once.
        std::wstring err;
        std::wstring server = resolveRoomServer(cfg.rooms.servers, cfg.rooms.server, &err);
        json::Value root = json::Value::object();
        root.set("server", narrow(server));
        json::Value games = json::Value::object();
        std::map<std::string, std::string> asked;  // online id -> its answer as JSON
        for (const auto& g : cfg.games) {
            if (g.onlineId.empty() || g.comingSoon) continue;
            auto it = asked.find(g.onlineId);
            if (it == asked.end()) {
                std::vector<RoomInfo> rooms;
                std::wstring e;
                bool rejected = false;
                bool listed = !server.empty() && listRooms(server, g.onlineId, rooms, e, 4000, &rejected);
                it = asked.emplace(g.onlineId, roomsJson(server, !rejected, rooms, listed ? std::wstring() : (e.empty() ? err : e))).first;
            }
            std::string parseError;
            games.set(g.id, json::parse(it->second, &parseError));
        }
        root.set("games", std::move(games));
        out(widen(json::serialize(root, false)) + L"\n");
        return 0;
    }
    if (!a.rooms.empty()) {
        const GameSpec* g = cfg.findGame(a.rooms);
        if (!g || g->onlineId.empty()) {
            fail(L"No online game with id \"" + widen(a.rooms) + L"\"");
            return 3;
        }
        std::wstring err;
        std::wstring server = resolveRoomServer(cfg.rooms.servers, cfg.rooms.server, &err);
        std::vector<RoomInfo> rooms;
        bool rejected = false;
        bool listed = !server.empty() && listRooms(server, g->onlineId, rooms, err, 4000, &rejected);
        if (a.json) {
            out(widen(roomsJson(server, !rejected, rooms, listed ? std::wstring() : err)) + L"\n");
            return listed || rejected ? 0 : 1;
        }
        if (!listed && rejected) {
            out(server + L": not on the server yet (" + err + L")\n");
            return 0;
        }
        if (server.empty() || (rooms.empty() && !err.empty())) {
            fail(L"Cannot list rooms: " + err);
            return 1;
        }
        out(server + L": " + std::to_wstring(rooms.size()) + L" open\n");
        for (const auto& r : rooms)
            out(r.code + L"  " + std::to_wstring(r.players) + L"/" + std::to_wstring(r.max) + L"  " + r.name + L"\n");
        return 0;
    }

    if (a.mods || !a.installMod.empty() || !a.repairMod.empty() || !a.uninstallMod.empty()) return runModCommand(a, cfg, lib, user, userPath);

    if (a.updatePlayers) {
        // What the window does at startup: a player installed from a GitHub release (Ruffle Together) is kept on
        // the newest one, because everyone in a co-op session has to run the same build.
        json::Value list = json::Value::array();
        int failed = 0;
        for (const auto& p : cfg.players) {
            // A front end draws its own progress bar: one line per whole percent while a release downloads.
            int lastPct = -1;
            InstallProgress progress;
            if (a.json) {
                progress = [&](const InstallStatus& s) {
                    if (s.stage != InstallStatus::Stage::Downloading || !s.total) return;
                    int pct = static_cast<int>(s.done * 100 / s.total);
                    if (pct == lastPct) return;
                    lastPct = pct;
                    json::Value pr = json::Value::object();
                    pr.set("id", p.id);
                    pr.set("done", static_cast<double>(s.done));
                    pr.set("total", static_cast<double>(s.total));
                    json::Value line = json::Value::object();
                    line.set("progress", std::move(pr));
                    out(widen(json::serialize(line, false)) + L"\n");
                };
            }
            PlayerUpdate u = updateInstalledPlayer(p, lib.vars(), nullptr, !a.check, progress);
            if (!u.error.empty()) failed++;
            if (a.json) {
                json::Value o = json::Value::object();
                o.set("id", p.id);
                o.set("checked", u.checked);
                o.set("available", u.available);
                o.set("updated", u.updated);
                o.set("installed", narrow(u.installed));
                o.set("version", narrow(u.version));
                o.set("error", narrow(u.error));
                list.push(std::move(o));
            } else if (u.updated) {
                out(p.name + L" updated to " + u.version + L"\n");
            } else if (u.available) {
                out(p.name + L" " + u.version + L" is available (installed: " + (u.installed.empty() ? std::wstring(L"unknown") : u.installed) + L")\n");
            } else if (u.checked) {
                out(p.name + L" is up to date (" + u.version + L")\n");
            } else if (!u.error.empty()) {
                out(p.name + L": " + u.error + L"\n");
            }
        }
        if (a.json) {
            json::Value root = json::Value::object();
            root.set("players", std::move(list));
            out(widen(json::serialize(root, false)) + L"\n");
        }
        return failed ? 1 : 0;
    }
    if (a.update) {
        if (!cfg.updates.enabled()) {
            fail(L"launcher.json has no \"updates\" source");
            return 3;
        }
        removeOldUpdate(fs::selfExePath());
        UpdateResult r = updateSelf(cfg.updates.source, LAUNCHER_VERSION, fs::selfExePath(), nullptr);
        if (!r.ok) {
            fail(L"Update failed: " + r.error);
            return 1;
        }
        out(r.updated ? L"Updated to " + r.version + L". It starts next time.\n"
                      : L"Up to date (" + std::wstring(LAUNCHER_VERSION) + L"; newest release " + r.version + L").\n");
        return 0;
    }

    if (!a.installPlayer.empty()) {
        const PlayerSpec* p = cfg.findPlayer(a.installPlayer);
        if (!p || !p->install.enabled()) {
            fail(L"No installable player with id \"" + widen(a.installPlayer) + L"\" (it needs an \"install\" entry)");
            return 3;
        }
        int lastPct = -1;
        InstallResult r = installPlayer(*p, lib.vars(), [&](const InstallStatus& s) {
            if (s.stage != InstallStatus::Stage::Downloading || !s.total) return;
            int pct = static_cast<int>(s.done * 100 / s.total);
            if (pct / 10 != lastPct / 10) out(std::to_wstring(pct) + L"%\n");
            lastPct = pct;
        }, nullptr);
        if (!r.ok) {
            fail(cfg.t("install.failed", {{L"PLAYER", p->name}, {L"ERROR", r.error}}));
            return 1;
        }
        out(cfg.t("install.done", {{L"PLAYER", r.version.empty() ? p->name : p->name + L" " + r.version}}) + L"\n" + r.exe + L"\n");
        return 0;
    }

    if (!a.downloadGame.empty()) {
        if (cfg.downloads.catalog.empty()) {
            fail(L"launcher.json has no downloads.catalog");
            return 3;
        }
        std::vector<CatalogFile> files, wanted;
        std::string raw;
        std::wstring err;
        if (!fetchCatalog(cfg.downloads.catalog, files, raw, err)) {
            fail(L"Cannot read the game catalog: " + err);
            return 1;
        }
        for (const auto& f : files)
            if (a.downloadGame == "all" || f.game == a.downloadGame) wanted.push_back(f);
        if (wanted.empty()) {
            fail(L"The catalog has no files for \"" + widen(a.downloadGame) + L"\"");
            return 3;
        }
        InstallResult r = downloadGameFiles(wanted, lib.vars().get(L"GAMES_DIR"), nullptr, nullptr);
        if (!r.ok) {
            fail(L"Download failed: " + r.error);
            return 1;
        }
        for (const auto& f : wanted) out(fs::join(lib.vars().get(L"GAMES_DIR"), f.name) + L"\n");
        return 0;
    }

    if (!a.lobbySession.empty()) {
        const GameSpec* g = cfg.findGame(a.lobbySession);
        if (!g) {
            fail(L"No game with id \"" + widen(a.lobbySession) + L"\" (see --list)");
            return 3;
        }
        return runLobbySession(cfg, lib, user, userPath, *g, a.tv);
    }

    if (a.list && a.json) {
        // As the window lists them: the website's copy from the last run adds its games and fills gaps.
        std::vector<SiteGame> siteList;
        if (!cfg.downloads.site.empty() && !getenv("FL_NO_SITE")) {
            SiteCache cached;
            if (loadSiteCache(fs::join(lib.dataDir(), L"site.json"), cached) && parseSite(cached.raw, siteList))
                mergeSite(lr.config, siteList, fs::join(lib.dataDir(), L"covers"));
        }
        out(widen(libraryJson(cfg, lib, user, &siteList)) + L"\n");
        return 0;
    }
    if (a.list) {
        for (const auto& p : lib.players())
            out(p.spec->name + L": " + (p.found() ? p.exe : std::wstring(L"not found")) + L"\n");
        out(L"Flashpoint: " + (lib.flashpointDir().empty() ? std::wstring(L"not found") : lib.flashpointDir()) + L"\n\n");
        for (const auto& g : cfg.games) {
            out(widen(g.id) + L"  " + g.name + L"\n");
            for (const auto& act : g.actions) {
                ResolvedAction r = lib.resolve(g, act, user);
                out(L"    " + widen(act.id) + L" (" + act.label + L"): " + stateWord(r.state) +
                    (r.player ? L", " + r.player->name : L"") + (r.swf.empty() ? L"" : L", " + r.swf) + L"\n");
            }
        }
        return 0;
    }

    if (!a.launch.empty()) {
        std::wstring gameId = a.launch, actionId;
        if (size_t colon = a.launch.find(L':'); colon != std::wstring::npos) {
            gameId = a.launch.substr(0, colon);
            actionId = a.launch.substr(colon + 1);
        }
        const GameSpec* g = cfg.findGame(narrow(gameId));
        if (!g) {
            fail(L"No game with id \"" + gameId + L"\" (see --list)");
            return 3;
        }
        if (g->comingSoon) {
            fail(g->name + L" is coming soon: its online mod isn't ready yet.");
            return 4;
        }
        const ActionSpec* act = g->actions.empty() ? nullptr : &g->actions.front();
        if (!actionId.empty()) {
            act = nullptr;
            for (const auto& x : g->actions)
                if (x.id == narrow(actionId)) act = &x;
        }
        if (!act) {
            fail(L"\"" + g->name + L"\" has no action \"" + actionId + L"\"");
            return 3;
        }
        std::wstring message;
        ui::Launcher::HeadlessOptions ho;
        for (wchar_t ch : a.join)
            if (iswalnum(ch)) ho.joinCode += static_cast<wchar_t>(towupper(ch));
        ho.address = a.address;
        ho.wait = a.wait;
        ho.tv = a.tv;
        if (!a.coopSave.empty() && a.coopSave != L"continue" && a.coopSave != L"solo" && a.coopSave != L"new") {
            fail(L"--coop-save takes continue, solo or new");
            return 3;
        }
        ho.coopSave = a.coopSave == L"solo" ? 1 : a.coopSave == L"new" ? 2 : 0;
        if (a.json) ho.report = [](const std::string& line) { out(widen(line) + L"\n"); };  // the hosting box's updates
        int code = ui::Launcher::launchHeadless(cfg, lib, user, userPath, *g, *act, message, ho);
        if (code) fail(message);
        else out(message + L"\n");
        return code;
    }

    // The app that replaced this window (launcher.json "updates": "app", "appExe"): once it is installed, this
    // launcher only opens it, so an old shortcut or folder still leads to the launcher players have now. The window
    // itself installs the app on its first start (Launcher::startUpdateCheck). Snapshots and the other test views
    // keep the window.
    if (cfg.updates.handsOver() && a.snapshot.empty() && a.select.empty() && !a.demoHost && !a.guide && a.lobby.empty()) {
        std::wstring app = fs::normalize(base.expand(cfg.updates.appExe));
        sys::Started p;
        std::wstring err;
        if (fs::isFile(app)) {
            DeleteFileW(fs::longPath(fs::join(dataDir, L"MultiplayerEverything-setup.exe")).c_str());  // done its job
            if (sys::startProcess(app, {}, fs::parent(app), p, err)) {
                CloseHandle(p.process);
                logger().info(L"Opened " + app + L", which replaces this window");
                logger().close();
                return 0;
            }
            logger().warn(L"Can't open " + app + L": " + err);
        }
    }

    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    Gdiplus::GdiplusStartupInput gin;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gin, nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // file dialog, Explorer reveal
    int code;
    {
        // Games the website lists that this launcher doesn't have yet become "Coming soon" cards, and the
        // website fills in what a game leaves out. The copy from the last run is used; the very first run
        // asks the website once, here.
        if (!lr.config.downloads.site.empty() && !getenv("FL_NO_SITE")) {
            std::vector<SiteGame> siteList;
            std::wstring siteCache = fs::join(lib.dataDir(), L"site.json"), siteErr;
            SiteCache cached;
            if (loadSiteCache(siteCache, cached) ? parseSite(cached.raw, siteList)
                                                 : loadSite(lr.config.downloads.site, siteCache, siteList, siteErr))
                mergeSite(lr.config, siteList, fs::join(lib.dataDir(), L"covers"));
        }
        ui::Launcher launcher(cfg, lib, user, userPath);
        ui::LauncherOptions opt;
        opt.snapshot = a.snapshot.empty() ? std::wstring() : fullPath(a.snapshot);
        opt.select = a.select;
        opt.width = a.width;
        opt.height = a.height;
        opt.demoHost = a.demoHost;
        opt.guide = a.guide;
        opt.guideScroll = a.guideScroll;
        opt.lobby = narrow(a.lobby);
        code = launcher.run(opt);
    }
    CoUninitialize();
    Gdiplus::GdiplusShutdown(gdiToken);
    logger().close();
    return code;
}
