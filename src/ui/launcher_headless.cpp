// Starting an action without the window (command line --launch).
#include "launcher.h"

#include <algorithm>
#include <cwctype>
#include <regex>

#include "core/discord.h"
#include "core/embedded.h"
#include "core/fs.h"
#include "core/json.h"
#include "core/log.h"
#include "core/motion.h"
#include "core/rooms.h"
#include "core/strings.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

// ---- headless launch -----------------------------------------------------

int Launcher::launchHeadless(const Config& cfg, Library& lib, UserData& user, const std::wstring& userPath,
                             const GameSpec& game, const ActionSpec& action, std::wstring& message,
                             const HeadlessOptions& opt) {
    const std::wstring& joinCode = opt.joinCode;
    const std::wstring& address = opt.address;
    if (game.mod) {
        mod::Spec spec = *game.mod;
        spec.text = &cfg.text;
        const GameStats* saved = user.find(game.id);
        mod::Status s = mod::inspect(spec, lib.vars(), saved ? saved->gameDir : L"");
        Vars vars = mod::withSpec(lib.vars(), spec);
        if (!s.game.found()) {
            message = spec.t("mod.status.noGame", vars);
            return 4;
        }
        if (!s.installed) {
            message = spec.t("mod.status.notInstalled", vars);
            return 4;
        }
        std::wstring err;
        if (!mod::launchGame(spec, lib.vars(), s.game.dir, err)) {
            message = cfg.t("notice.launchFailed", {{L"PLAYER", game.name}, {L"ERROR", err}});
            return 1;
        }
        GameStats& st = user.stats(game.id);
        ++st.plays;
        st.lastPlayed = unixNow();
        saveUserData(user, userPath);
        message = cfg.t("notice.launched", {{L"GAME", game.name}, {L"PLAYER", game.name}});
        return 0;
    }
    if (action.isLink()) {
        message = L"\"" + action.label + L"\" opens " + action.link + L" in your browser and needs the launcher window.";
        return 3;
    }
    ResolvedAction r = lib.resolve(game, action, user, !joinCode.empty());
    if (r.state != ActionState::Ready) {
        message = cfg.t("notice.notReady", {{L"GAME", game.name}, {L"REASON", stateReason(cfg, r)}});
        return 4;
    }
    Relay relay;
    if (action.relay.enabled) {
        std::wstring err;
        bool inUse = false;
        if (!relay.start(action.relay.port, err, &inUse) && !inUse) {
            message = cfg.t("host.failed", {{L"PORT", std::to_wstring(action.relay.port)}, {L"ERROR", err}});
            return 1;
        }
    }
    // HTML5: the local web server or the game's own server comes first.
    sys::BackgroundProcess server;
    WebServer web;
    if (r.web) {
        std::wstring url = r.url, err;
        if (action.joinLink) {
            message = L"\"" + action.label + L"\" opens a friend's link and needs the launcher window.";
            return 3;
        }
        if (r.pageLocal) {
            if (!web.start(fs::parent(r.page), stablePort(game.id), err) && !web.start(fs::parent(r.page), 0, err)) {
                message = cfg.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}});
                return 1;
            }
            url = L"http://127.0.0.1:" + std::to_wstring(web.port()) + L"/" + fs::fileName(r.page);
        }
        if (action.server.enabled()) {
            if (!server.start(r.serverExe, r.serverArgs, r.serverCwd,
                              [](const std::wstring& line) { logger().info(L"[server] " + line); }, err)) {
                message = cfg.t("notice.launchFailed", {{L"PLAYER", action.server.name}, {L"ERROR", err}});
                return 1;
            }
            if (!sys::waitForServer(url, 30000, [&server] { return server.running(); })) {
                message = cfg.t("web.serverTimeout", {{L"GAME", game.name}});
                return 1;
            }
        }
        for (auto& arg : r.args) arg = replaceAll(arg, L"${URL}", url);
    }
    if (!r.web) {
        // No window: the room code and address come from --join and --address; the server is the central one.
        std::wstring roomServer = resolveRoomServer(cfg.rooms.servers, cfg.rooms.server, nullptr);
        if (roomServer.empty()) roomServer = cfg.rooms.server;
        std::wstring missing = fillPlaceholders(r, user.playerName, joinCode, roomServer, address);
        if (!missing.empty()) {
            message = L"\"" + action.label + L"\" needs a " + (missing == L"CODE" ? L"room code" : L"value for ${" + missing + L"}") +
                      L" and needs the launcher window.";
            return 3;
        }
        // The host's save choice and the game's settings, as the window adds them.
        auto catalogSettings = cachedCatalogSettings(lib.dataDir());
        GameOptions(cfg, lib, user, catalogSettings).addHostOptions(game, r.args, opt.coopSave);
        FlashVars given = opt.flashVars;
        if (!joinCode.empty()) given.push_back({L"join", joinCode});
        addFlashVars(r, gameValues(action, user.playerName, given));
    }
    sys::EnvVars env;
    if (opt.tv && r.player && !r.web) {
        r.args.insert(r.args.end(), r.player->tvArgs.begin(), r.player->tvArgs.end());
        env = r.player->tvEnv;
    }
    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(r.playerExe, r.args, r.workDir, p, err, false, env)) {
        message = cfg.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}});
        return 1;
    }
    GameStats& st = user.stats(game.id);
    ++st.plays;
    st.lastPlayed = unixNow();
    saveUserData(user, userPath);
    message = action.hint.empty() ? cfg.t("notice.launched", {{L"GAME", game.name}, {L"PLAYER", r.player->name}})
                                  : action.hint;
    if (opt.process) {  // the caller watches the game (and counts its time)
        *opt.process = p.process;
        return 0;
    }
    if (relay.running() || r.web || opt.wait) {  // the relay or the game's server must outlive the game
        ULONGLONG start = GetTickCount64();
        // "Playing <game>" on the player's Discord profile while it runs, as the window shows it.
        std::unique_ptr<DiscordPresence> presence;
        if (!cfg.discord.clientId.empty() && !getenv("FL_NO_DISCORD")) {
            presence = std::make_unique<DiscordPresence>(cfg.discord.clientId);
            Presence pr;
            pr.details = cfg.t("discord.playing", {{L"GAME", game.name}});
            pr.state = !joinCode.empty() ? cfg.t("discord.room", {{L"CODE", joinCode}}) : action.label;
            pr.start = unixNow();
            presence->set(pr);
        }
        if (opt.report && relay.running()) {
            // The hosting box's view: where friends connect, and who has.
            std::string last;
            do {
                json::Value o = json::Value::object();
                json::Value rel = json::Value::object();
                rel.set("port", relay.port());
                json::Value addrs = json::Value::array();
                for (const auto& a : sys::localAddresses()) {
                    json::Value ao = json::Value::object();
                    ao.set("address", narrow(a.address));
                    ao.set("adapter", narrow(a.adapter));
                    addrs.push(std::move(ao));
                }
                rel.set("addresses", std::move(addrs));
                json::Value peers = json::Value::array();
                for (const auto& peer : relay.snapshot().peers) {
                    json::Value po = json::Value::object();
                    po.set("name", narrow(peer.name));
                    po.set("address", narrow(peer.address));
                    po.set("host", peer.host);
                    peers.push(std::move(po));
                }
                rel.set("peers", std::move(peers));
                o.set("relay", std::move(rel));
                std::string line = json::serialize(o, false);
                if (line != last) opt.report(line);
                last = std::move(line);
            } while (WaitForSingleObject(p.process, 1000) == WAIT_TIMEOUT);
        } else {
            WaitForSingleObject(p.process, INFINITE);
        }
        // A front end (the Tauri window) writes settings while the game runs: add the time to what is
        // on disk now instead of writing back the copy read at launch.
        int64_t played = static_cast<int64_t>((GetTickCount64() - start) / 1000);
        user = loadUserData(userPath);
        user.stats(game.id).seconds += played;
        saveUserData(user, userPath);
    }
    CloseHandle(p.process);
    return 0;
}

}  // namespace fl::ui
