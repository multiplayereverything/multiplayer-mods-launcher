// The launcher window: starting games, installing players, downloading game files, hosting.
#include "launcher.h"

#include <algorithm>
#include <cwctype>
#include <regex>

#include "core/embedded.h"
#include "core/fs.h"
#include "core/install.h"
#include "core/log.h"
#include "core/motion.h"
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

// ---- actions --------------------------------------------------------------

void Launcher::notice(const std::wstring& text, const Color& color) {
    notice_ = text;
    noticeColor_ = color;
    noticeUntil_ = GetTickCount64() + 10000;
    logger().info(L"Notice: " + text);
    InvalidateRect(hwnd_, &status_, FALSE);
}

bool Launcher::isPlaying(int gi) const {
    for (const auto& s : sessions_)
        if (s.game == gi) return true;
    return false;
}

bool Launcher::minimizeOnLaunch() const {
    return user_.minimize >= 0 ? user_.minimize != 0 : cfg_.behavior.minimizeOnLaunch;
}

bool Launcher::runAction(int gi, size_t ai, const std::vector<std::pair<std::wstring, std::wstring>>& given) {
    if (gi < 0 || ai >= cfg_.games[gi].actions.size()) return false;
    const GameSpec& g = cfg_.games[gi];
    const ActionSpec& a = g.actions[ai];
    if (a.isMod()) return runModAction(gi, a);
    if (a.isLink()) {
        if (!sys::openPath(a.link)) {
            notice(cfg_.t("notice.launchFailed", {{L"PLAYER", g.name}, {L"ERROR", a.link}}), theme_.c.error);
            return false;
        }
        return true;
    }
    // A mod on the launcher's lobby plays online through the lobby window; the
    // lobby calls back here with the room's values once the host starts.
    if (usesLobby(gi) && ai == joinActionIndex(g) && given.empty()) {
        openLobby(gi);
        return true;
    }
    FlashVars flashvars = gameValues(a, user_.playerName, given);
    std::wstring code;
    for (const auto& [key, value] : given)
        if (key == L"join") code = value;
    ResolvedAction r = lib_.resolve(g, a, user_, !code.empty());
    if (r.state != ActionState::Ready) {
        notice(cfg_.t("notice.notReady", {{L"GAME", g.name}, {L"REASON", stateReason(cfg_, r)}}), theme_.c.warning);
        if (r.state == ActionState::NoSwf) SetFocus(IsWindowVisible(downloadBtn_) ? downloadBtn_ : locateBtn_);
        else if (r.state == ActionState::NoPlayer || r.state == ActionState::NoRuntime) SetFocus(getPlayerBtn_);
        return false;
    }
    if (r.player && r.player->kind == "window") {
        notice(L"\"" + a.label + L"\" opens in the new launcher's own window.", theme_.c.warning);
        return false;
    }
    if (r.web) return runWeb(gi, a, r, flashvars);
    // A player that takes its room and name on the command line (Ruffle Together's --coop-* options).
    {
        std::wstring address, server;
        bool wantsAddress = false;
        for (const auto& arg : r.args)
            if (arg.find(L"${ADDRESS}") != std::wstring::npos) wantsAddress = true;
        if (wantsAddress) {
            // The clipboard is only a guess for the field: friends usually copy the address before pressing this.
            if (!pendingAddress_.empty()) {
                address = pendingAddress_;  // the Join box already took it
            } else {
                address = clipboardText();
                if (!askAddress(address, a.label)) return false;
            }
        }
        {
            std::lock_guard<std::mutex> lock(installMu_);
            server = roomServer_.empty() ? cfg_.rooms.server : roomServer_;
        }
        std::wstring missing = fillPlaceholders(r, user_.playerName, code, server, address);
        if (!missing.empty()) {
            notice(cfg_.t(missing == L"CODE" ? "notice.needCode" : "notice.needServer", {{L"ACTION", a.label}}),
                   theme_.c.warning);
            return false;
        }
    }
    // The host's own choices: which co-op save to start from, and the game's host options.
    gameOptions().addHostOptions(g, r.args, coopSaveChoiceFor(g));
    addFlashVars(r, flashvars);
    bool startedRelay = false;
    std::wstring warning;
    if (a.relay.enabled && !(relay_.running() && relay_.port() == a.relay.port)) {
        std::wstring err;
        bool inUse = false;
        if (relay_.start(a.relay.port, err, &inUse)) {
            startedRelay = true;
            relayGame_ = gi;
            addresses_ = sys::localAddresses();
        } else if (inUse) {
            // Most likely the mod's own relay script is already running; the game works with it.
            warning = cfg_.t("host.portInUse", {{L"PORT", std::to_wstring(a.relay.port)}});
        } else {
            notice(cfg_.t("host.failed", {{L"PORT", std::to_wstring(a.relay.port)}, {L"ERROR", err}}), theme_.c.error);
            return false;
        }
    } else if (a.relay.enabled) {
        relayGame_ = gi;
    }

    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(r.playerExe, r.args, r.workDir, p, err)) {
        if (startedRelay) stopRelay(false);
        notice(cfg_.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}}), theme_.c.error);
        return false;
    }
    Session s;
    s.id = ++nextSessionId_;
    s.pid = p.pid;
    s.game = gi;
    s.action = a.id;
    s.player = r.player->name;
    s.started = GetTickCount64();
    s.startedUnix = unixNow();
    for (const auto& [key, value] : given)
        if (key == L"join") s.room = value;
    s.host = a.relay.enabled;
    sessions_.push_back(s);
    watcher_->watch(p.process, p.pid);
    updatePresence();

    GameStats& st = user_.stats(g.id);
    ++st.plays;
    st.lastPlayed = unixNow();
    save();
    std::wstring msg = warning.empty() ? (a.hint.empty() ? cfg_.t("notice.launched", {{L"GAME", g.name}, {L"PLAYER", r.player->name}}) : a.hint)
                                       : warning;
    notice(msg, warning.empty() ? theme_.c.success : theme_.c.warning);
    buildNav();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    if (minimizeOnLaunch()) ShowWindow(hwnd_, SW_MINIMIZE);
    return true;
}

void Launcher::onProcessExit(DWORD pid, DWORD code) {
    if (pid && pid == lobbyPid_) {
        lobbyPid_ = 0;
        if (lobby_) lobby_->gameExited();
    }
    auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.pid == pid; });
    if (it == sessions_.end()) return;
    Session s = *it;
    endSession(it);
    int64_t secs = static_cast<int64_t>((GetTickCount64() - s.started) / 1000);
    const GameSpec& g = cfg_.games[s.game];
    user_.stats(g.id).seconds += secs;
    save();
    logger().info(g.name + L" (" + s.player + L") exited with code " + std::to_wstring(code) + L" after " +
                  std::to_wstring(secs) + L" s");
    if (code != 0 && secs < cfg_.behavior.quickExitSeconds)
        notice(cfg_.t("notice.quitFast", {{L"PLAYER", s.player}, {L"CODE", std::to_wstring(code)}}), theme_.c.error);
    if (s.host && !s.server && cfg_.behavior.stopRelayWithGame && relay_.running()) {
        bool otherHost =
            std::any_of(sessions_.begin(), sessions_.end(), [](const Session& x) { return x.host && !x.server; });
        if (!otherHost) stopRelay(true);
    }
    if (sessions_.empty() && IsIconic(hwnd_) && minimizeOnLaunch()) ShowWindow(hwnd_, SW_RESTORE);
    updatePresence();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    InvalidateRect(hwnd_, &status_, FALSE);
}

void Launcher::stopRelay(bool announce) {
    relay_.stop();
    relayGame_ = -1;
    slotPeers_ = 0;
    joinFrom_.clear();
    if (announce) notice(cfg_.t("notice.relayStopped"), theme_.c.textMuted);
    layoutDetail();
}

void Launcher::locateFile() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    if (g.mod) {  // a PC mod: the button asks for the game's folder
        chooseGameFolder();
        return;
    }
    const ActionSpec* target = nullptr;
    for (const auto& a : g.actions)
        if (!target && lib_.resolve(g, a, user_).state == ActionState::NoSwf) target = &a;
    if (!target && !g.actions.empty()) target = &g.actions.front();
    if (!target) return;
    wchar_t file[MAX_PATH * 4] = L"";
    std::wstring filter = cfg_.t("dialog.swfFilter");
    filter.push_back(L'\0');
    filter += L"*.swf";
    filter.push_back(L'\0');
    filter.push_back(L'\0');
    std::wstring title = cfg_.t("dialog.locateTitle", {{L"GAME", g.name}});
    std::wstring initial = lib_.flashpointDir().empty() ? lib_.vars().get(L"GAMES_DIR") : lib_.flashpointDir();
    OPENFILENAMEW ofn{sizeof ofn};
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = title.c_str();
    ofn.lpstrInitialDir = initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    user_.stats(g.id).swf[target->id] = fs::normalize(file);
    save();
    notice(cfg_.t("notice.located", {{L"GAME", g.name}, {L"FILE", fs::fileName(file)}}), theme_.c.success);
    buildNav();
    rebuildGrid();
    select(selected_);
}

void Launcher::openFolder() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    if (g.mod) {  // the game's own folder, when it has been found
        if (const ModView* v = modView(selected_); v && v->status.game.found()) sys::openPath(v->status.game.dir);
        return;
    }
    for (const auto& a : g.actions) {
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (!r.swf.empty() && !r.swfIsUrl) {
            sys::revealInExplorer(r.swf);
            return;
        }
    }
    std::wstring dir = lib_.vars().get(L"GAMES_DIR");
    fs::ensureDir(dir, nullptr, nullptr);
    sys::openPath(dir);
}

void Launcher::getPlayer() {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    for (const auto& a : g.actions) {
        ResolvedAction r = lib_.resolve(g, a, user_);
        if (r.state == ActionState::NoRuntime && !a.server.website.empty()) {
            sys::openPath(a.server.website);
            return;
        }
        if (r.state == ActionState::NoPlayer && r.player && r.player->install.enabled()) {
            offerInstall(*r.player, false);
            return;
        }
        if (r.state == ActionState::NoPlayer && r.player && !r.player->website.empty()) {
            sys::openPath(r.player->website);
            return;
        }
    }
    sys::openPath(lib_.vars().get(L"PLAYERS_DIR"));
}

void Launcher::offerInstall(const PlayerSpec& player, bool firstRun) {
    if (installing_ || snapshot_) return;
    const InstallSpec& in = player.install;
    std::wstring source = !in.github.empty() ? L"github.com/" + widen(in.github) : in.url;
    std::wstring dir = lib_.vars().expand(in.dir);
    std::wstring title = cfg_.t("install.title", {{L"PLAYER", player.name}});
    std::wstring body = cfg_.t("install.body", {{L"PLAYER", player.name}, {L"SOURCE", source}, {L"DIR", dir}});
    if (firstRun) body = cfg_.t("install.firstRun") + L"\n\n" + body;
    std::wstring yes = cfg_.t("install.yes", {{L"PLAYER", player.name}}), no = cfg_.t("install.no");
    TASKDIALOG_BUTTON buttons[] = {{IDYES, yes.c_str()}, {IDNO, no.c_str()}};
    TASKDIALOGCONFIG tdc{sizeof tdc};
    tdc.hwndParent = hwnd_;
    tdc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = cfg_.branding.subtitle.c_str();
    tdc.pszMainInstruction = title.c_str();
    tdc.pszContent = body.c_str();
    tdc.cButtons = 2;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = IDYES;
    int choice = 0;
    if (FAILED(TaskDialogIndirect(&tdc, &choice, nullptr, nullptr)) || choice != IDYES) return;

    if (installThread_.joinable()) installThread_.join();
    installing_ = true;
    installCancel_ = false;
    installPlayer_ = &player;
    jobIsGame_ = false;
    jobName_ = player.name;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        installStatus_ = {};
    }
    Vars vars = lib_.vars();
    HWND wnd = hwnd_;
    installThread_ = std::thread([this, &player, vars, wnd] {
        InstallResult r = installPlayer(
            player, vars,
            [this, wnd](const InstallStatus& s) {
                std::lock_guard<std::mutex> lock(installMu_);
                // Downloads report every 256 KB; repaint at most for each new percent.
                bool post = s.stage != installStatus_.stage ||
                            (s.total && s.done * 100 / s.total != installStatus_.done * 100 / std::max<uint64_t>(installStatus_.total, 1));
                installStatus_ = s;
                if (post) PostMessageW(wnd, WM_APP_INSTALL, 0, 0);
            },
            &installCancel_);
        {
            std::lock_guard<std::mutex> lock(installMu_);
            installResult_ = r;
        }
        PostMessageW(wnd, WM_APP_INSTALLED, 0, 0);
    });
    layoutDetail();
    InvalidateRect(hwnd_, &status_, FALSE);
}

std::wstring Launcher::installText() const {
    if (!installing_) return {};
    if (modJobGame_ >= 0) return modJobText();
    std::lock_guard<std::mutex> lock(installMu_);
    const std::wstring& name = jobName_;
    if (jobIsGame_)
        return cfg_.t("download.downloading", {{L"GAME", name},
                                               {L"DONE", formatBytes(installStatus_.done)},
                                               {L"TOTAL", installStatus_.total ? formatBytes(installStatus_.total) : L"?"}});
    switch (installStatus_.stage) {
        case InstallStatus::Stage::Downloading:
            return cfg_.t("install.downloading", {{L"PLAYER", name},
                                                  {L"DONE", formatBytes(installStatus_.done)},
                                                  {L"TOTAL", installStatus_.total ? formatBytes(installStatus_.total) : L"?"}});
        case InstallStatus::Stage::Verifying:
        case InstallStatus::Stage::Unpacking: return cfg_.t("install.unpacking", {{L"PLAYER", name}});
        default: return cfg_.t("install.finding", {{L"PLAYER", name}});
    }
}

void Launcher::onInstalled() {
    if (installThread_.joinable()) installThread_.join();
    installing_ = false;
    InstallResult r;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        r = installResult_;
    }
    std::wstring name = jobName_;
    refreshAll();
    if (jobIsGame_) {
        checkOutdated();
        select(selected_);
        InvalidateRect(grid_, nullptr, FALSE);
        if (r.ok) toast(cfg_.t("download.done", {{L"GAME", name}}), theme_.c.success);
        else notice(cfg_.t("download.failed", {{L"GAME", name}, {L"ERROR", r.error}}), theme_.c.error);
        return;
    }
    if (r.ok)
        toast(cfg_.t("install.done", {{L"PLAYER", r.version.empty() ? name : name + L" " + r.version}}), theme_.c.success);
    else
        notice(cfg_.t("install.failed", {{L"PLAYER", name}, {L"ERROR", r.error}}), theme_.c.error);
}

void Launcher::loadCatalog() {
    if (cfg_.downloads.catalog.empty()) return;
    // Show what the last session knew at once, then refresh in the background.
    std::wstring cache = fs::join(lib_.dataDir(), L"catalog.json");
    std::string text;
    std::wstring err;
    if (fs::readText(cache, text)) {
        parseCatalog(text, cfg_.downloads.catalog, catalog_, err);
        parseCatalogSettings(text, catalogSettings_);
    }
    if (snapshot_ || !user_.online) {
        checkOutdated();  // the network stays off: compare against the cached list
        select(selected_);
        return;
    }
    std::wstring url = cfg_.downloads.catalog;
    HWND wnd = hwnd_;
    std::vector<CatalogFile> cached = catalog_;
    std::wstring gamesDir = lib_.vars().get(L"GAMES_DIR");
    catalogThread_ = std::thread([this, url, wnd, cached, gamesDir] {
        // Hashing the downloaded files takes a moment, so it happens here too:
        // first against the cached list, then against the fresh one.
        auto publishOutdated = [&](const std::vector<CatalogFile>& list) {
            std::set<std::wstring> names;
            for (const auto& f : outdatedGameFiles(list, gamesDir)) names.insert(f.name);
            std::lock_guard<std::mutex> lock(installMu_);
            pendingOutdated_ = std::move(names);
            pendingOutdatedReady_ = true;
        };
        publishOutdated(cached);
        PostMessageW(wnd, WM_APP_CATALOG, 0, 0);
        std::vector<CatalogFile> files;
        std::string raw;
        std::wstring error;
        if (fetchCatalog(url, files, raw, error, &installCancel_)) {
            publishOutdated(files);
            {
                std::lock_guard<std::mutex> lock(installMu_);
                catalogRaw_ = std::move(raw);
            }
            PostMessageW(wnd, WM_APP_CATALOG, 0, 0);
        } else {
            logger().warn(L"Game catalog not refreshed: " + error);
        }
    });
}

std::vector<CatalogFile> Launcher::missingGameFiles(const GameSpec& game) const {
    // Only files a shown button can use: hidden originals and builds for players
    // the game doesn't allow aren't worth the download.
    std::set<std::wstring> wanted;
    for (const auto& a : game.actions) {
        if (!actionShown(a)) continue;
        for (const auto& s : a.swf) wanted.insert(toLower(fs::fileName(s)));
        for (const auto& [pid, o] : a.overrides)
            if (a.players.empty() || std::find(a.players.begin(), a.players.end(), pid) != a.players.end())
                for (const auto& s : o.swf) wanted.insert(toLower(fs::fileName(s)));
    }
    std::vector<CatalogFile> out;
    std::wstring dir = lib_.vars().get(L"GAMES_DIR");
    for (const auto& f : catalog_)
        if (f.game == game.id && wanted.count(toLower(f.name)) && !fs::isFile(fs::join(dir, f.name))) out.push_back(f);
    return out;
}

void Launcher::offerGameDownload(const GameSpec& game) {
    if (installing_ || snapshot_) return;
    std::vector<CatalogFile> files = missingGameFiles(game);
    if (files.empty()) files = outdatedFor(game);
    if (files.empty()) return;
    uint64_t bytes = 0;
    for (const auto& f : files) bytes += f.size;
    std::wstring dir = lib_.vars().get(L"GAMES_DIR");
    std::wstring source = cfg_.downloads.catalog;
    // Name the repository, not the raw file host, so people know where it comes from.
    if (startsWithI(source, L"https://raw.githubusercontent.com/")) {
        std::vector<std::wstring> parts = split(source.substr(34), L'/');
        if (parts.size() >= 2) source = L"github.com/" + parts[0] + L"/" + parts[1];
    }
    std::wstring title = cfg_.t("download.title", {{L"GAME", game.name}});
    std::wstring body = cfg_.t("download.body", {{L"COUNT", std::to_wstring(files.size())},
                                                 {L"SIZE", formatBytes(bytes)},
                                                 {L"SOURCE", source},
                                                 {L"DIR", dir}});
    std::wstring yes = cfg_.t("download.yes"), no = cfg_.t("install.no");
    TASKDIALOG_BUTTON buttons[] = {{IDYES, yes.c_str()}, {IDNO, no.c_str()}};
    TASKDIALOGCONFIG tdc{sizeof tdc};
    tdc.hwndParent = hwnd_;
    tdc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
    tdc.pszWindowTitle = cfg_.branding.subtitle.c_str();
    tdc.pszMainInstruction = title.c_str();
    tdc.pszContent = body.c_str();
    tdc.cButtons = 2;
    tdc.pButtons = buttons;
    tdc.nDefaultButton = IDYES;
    int choice = 0;
    if (FAILED(TaskDialogIndirect(&tdc, &choice, nullptr, nullptr)) || choice != IDYES) return;

    if (installThread_.joinable()) installThread_.join();
    installing_ = true;
    installCancel_ = false;
    jobIsGame_ = true;
    jobName_ = game.name;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        installStatus_ = {};
    }
    HWND wnd = hwnd_;
    std::wstring catalogUrl = cfg_.downloads.catalog;
    installThread_ = std::thread([this, files, dir, wnd, catalogUrl]() mutable {
        // The list loaded at startup can be hours old: a game updated since then has a new size and
        // checksum, and downloading it against the old ones would always fail. Use the live entries.
        {
            std::vector<CatalogFile> fresh;
            std::string raw;
            std::wstring err;
            if (fetchCatalog(catalogUrl, fresh, raw, err, &installCancel_)) {
                for (auto& f : files)
                    for (const auto& n : fresh)
                        if (n.game == f.game && toLower(n.name) == toLower(f.name)) f = n;
                std::lock_guard<std::mutex> lock(installMu_);
                catalogRaw_ = std::move(raw);
            } else {
                logger().warn(L"Game catalog not refreshed before the download: " + err);
            }
        }
        InstallResult r = downloadGameFiles(
            files, dir,
            [this, wnd](const InstallStatus& s) {
                std::lock_guard<std::mutex> lock(installMu_);
                bool post = s.stage != installStatus_.stage ||
                            (s.total && s.done * 100 / s.total != installStatus_.done * 100 / std::max<uint64_t>(installStatus_.total, 1));
                installStatus_ = s;
                if (post) PostMessageW(wnd, WM_APP_INSTALL, 0, 0);
            },
            &installCancel_);
        {
            std::lock_guard<std::mutex> lock(installMu_);
            installResult_ = r;
        }
        PostMessageW(wnd, WM_APP_INSTALLED, 0, 0);
    });
    layoutDetail();
    InvalidateRect(hwnd_, &status_, FALSE);
}

std::wstring Launcher::clipboardText() {
    std::wstring text;
    // Another program (a clipboard manager, the one that just copied) may hold
    // the clipboard for a moment; wait briefly instead of reading nothing.
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(hwnd_) != FALSE); ++i) Sleep(20);
    if (!open) return text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return trim(text);
}

bool Launcher::runWeb(int gi, const ActionSpec& a, ResolvedAction r,
                      const std::vector<std::pair<std::wstring, std::wstring>>& flashvars) {
    const GameSpec& g = cfg_.games[gi];
    Session s;
    s.id = ++nextSessionId_;
    s.game = gi;
    s.action = a.id;
    s.player = r.player->name;
    s.exe = r.playerExe;
    s.args = r.args;
    s.workDir = r.workDir;
    s.url = r.url;
    if (a.joinLink) {
        // A friend's game: the link they sent, from the clipboard.
        std::wstring link = clipboardText();
        if (!Library::isUrl(link) || link.find_first_of(L" \t\r\n") != std::wstring::npos) {
            notice(cfg_.t("web.joinNeedsLink", {{L"ACTION", a.label}}), theme_.c.warning);
            return false;
        }
        s.url = link;
    }
    if (r.pageLocal) {
        s.web = std::make_shared<WebServer>();
        std::wstring err;
        if (!s.web->start(fs::parent(r.page), stablePort(g.id), err) && !s.web->start(fs::parent(r.page), 0, err)) {
            notice(cfg_.t("notice.launchFailed", {{L"PLAYER", r.player->name}, {L"ERROR", err}}), theme_.c.error);
            return false;
        }
        s.url = L"http://127.0.0.1:" + std::to_wstring(s.web->port()) + L"/" + fs::fileName(r.page);
    }
    // Values for the game (a room code) travel as query parameters.
    for (const auto& [key, value] : flashvars)
        s.url += std::wstring(s.url.find(L'?') == std::wstring::npos ? L"?" : L"&") + key + L"=" + value;

    GameStats& st = user_.stats(g.id);
    ++st.plays;
    st.lastPlayed = unixNow();
    save();
    if (!a.server.enabled()) {
        if (!launchBrowser(s)) return false;
        sessions_.push_back(s);
        notice(a.hint.empty() ? cfg_.t("notice.launched", {{L"GAME", g.name}, {L"PLAYER", s.player}}) : a.hint,
               theme_.c.success);
        select(selected_);
        InvalidateRect(grid_, nullptr, FALSE);
        return true;
    }
    // A server first (hidden, its output read for links), then the browser once
    // the game answers at its address.
    s.server = std::make_shared<sys::BackgroundProcess>();
    HWND wnd = hwnd_;
    int id = s.id;
    std::wstring err;
    if (!s.server->start(r.serverExe, r.serverArgs, r.serverCwd,
                         [wnd, id](const std::wstring& line) {
                             logger().info(L"[server] " + line);
                             PostMessageW(wnd, WM_APP_SERVERLINE, 0, reinterpret_cast<LPARAM>(new ServerLine{id, line}));
                         },
                         err)) {
        notice(cfg_.t("notice.launchFailed", {{L"PLAYER", a.server.name}, {L"ERROR", err}}), theme_.c.error);
        return false;
    }
    s.host = true;
    s.started = GetTickCount64();
    sessions_.push_back(s);
    std::shared_ptr<sys::BackgroundProcess> server = s.server;
    std::wstring url = r.url;
    helpers_.emplace_back([wnd, id, server, url] {
        bool ok = sys::waitForServer(url, 30000, [server] { return server->running(); });
        PostMessageW(wnd, WM_APP_SERVERREADY, static_cast<WPARAM>(id), ok ? 1 : 0);
    });
    notice(cfg_.t("web.starting", {{L"GAME", g.name}}), theme_.c.online);
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    return true;
}

bool Launcher::launchBrowser(Session& s) {
    std::vector<std::wstring> args;
    for (const auto& a : s.args) args.push_back(replaceAll(a, L"${URL}", s.url));
    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(s.exe, args, s.workDir, p, err)) {
        notice(cfg_.t("notice.launchFailed", {{L"PLAYER", s.player}, {L"ERROR", err}}), theme_.c.error);
        return false;
    }
    s.pid = p.pid;
    s.started = GetTickCount64();
    watcher_->watch(p.process, p.pid);
    if (minimizeOnLaunch()) ShowWindow(hwnd_, SW_MINIMIZE);
    return true;
}

void Launcher::onServerLine(int sessionId, const std::wstring& line) {
    auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.id == sessionId; });
    if (it == sessions_.end()) return;
    const ActionSpec* a = nullptr;
    for (const auto& x : cfg_.games[it->game].actions)
        if (x.id == it->action) a = &x;
    if (!a) return;
    bool changed = false;
    auto collect = [&](const std::wstring& pattern, std::vector<std::wstring>& into) {
        if (pattern.empty()) return;
        try {
            std::wregex re(pattern, std::regex::icase);
            for (std::wsregex_iterator m(line.begin(), line.end(), re), end; m != end; ++m) {
                std::wstring link = m->str();
                if (std::find(into.begin(), into.end(), link) == into.end()) {
                    into.push_back(link);
                    changed = true;
                }
            }
        } catch (const std::regex_error&) {
        }
    };
    collect(a->server.share, it->shareLinks);
    collect(a->server.lan, it->lanLinks);
    if (changed && it->game == selected_) layoutDetail();
}

void Launcher::onServerReady(int sessionId, bool ok) {
    auto it = std::find_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.id == sessionId; });
    if (it == sessions_.end()) return;
    const GameSpec& g = cfg_.games[it->game];
    if (!ok) {
        bool died = it->server && !it->server->running();
        endSession(it);
        notice(died ? cfg_.t("web.serverFailed", {{L"GAME", g.name}, {L"ERROR", L"see the log"}})
                    : cfg_.t("web.serverTimeout", {{L"GAME", g.name}}),
               theme_.c.error);
        select(selected_);
        return;
    }
    if (!launchBrowser(*it)) {
        endSession(it);
        select(selected_);
        return;
    }
    const ActionSpec* a = nullptr;
    for (const auto& x : g.actions)
        if (x.id == it->action) a = &x;
    notice(a && !a->hint.empty() ? a->hint : cfg_.t("notice.launched", {{L"GAME", g.name}, {L"PLAYER", it->player}}),
           theme_.c.success);
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
}

void Launcher::endSession(std::vector<Session>::iterator it) {
    if (it == sessions_.end()) return;
    // Stopping the server ends everything it started too (the tunnel); the local
    // web server closes with its last reference.
    if (it->server) it->server->stop();
    if (it->web) it->web->stop();
    sessions_.erase(it);
}

Launcher::Session* Launcher::webHostSession(int gameIndex) {
    for (auto& s : sessions_)
        if (s.game == gameIndex && s.server) return &s;
    return nullptr;
}

std::vector<Launcher::HostLine> Launcher::hostView() {
    std::vector<HostLine> out;
    if (Session* s = webHostSession(selected_)) {
        out.push_back({cfg_.t("web.serverRunning"), 0});
        out.push_back({cfg_.t("web.links"), 1});
        size_t shown = 0;
        for (const auto* list : {&s->shareLinks, &s->lanLinks})
            for (const auto& link : *list)
                if (shown++ < 3) out.push_back({link, 2});
        if (!shown) out.push_back({cfg_.t("web.waitingLink"), 1});
        return out;
    }
    if (!(relay_.running() && relayGame_ == selected_)) return out;
    std::wstring port = std::to_wstring(relay_.port());
    out.push_back({cfg_.t("host.running", {{L"PORT", port}}), 0});
    out.push_back({cfg_.t("host.addresses"), 1});
    if (addresses_.empty()) out.push_back({cfg_.t("host.noAddress", {{L"PORT", port}}), 2});
    for (size_t i = 0; i < addresses_.size() && i < 3; ++i)
        out.push_back({addresses_[i].address + L":" + port + L"   " + addresses_[i].adapter, 2});
    return out;
}

void Launcher::copyText(const std::wstring& text) {
    // Another program may hold the clipboard for a moment; wait briefly for it.
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(hwnd_) != FALSE); ++i) Sleep(20);
    if (!open) {
        logger().warn(L"Couldn't open the clipboard to copy " + text);
        return;
    }
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

std::wstring Launcher::roomCountText(const std::string& gameId) const {
    if (!onlineOn_ || !roomsKnown_) return {};
    auto it = rooms_.find(gameId);
    if (it == rooms_.end() || it->second.empty()) return {};
    return it->second.size() == 1 ? cfg_.t("rooms.one") : cfg_.t("rooms.count", {{L"N", std::to_wstring(it->second.size())}});
}

void Launcher::startRoomPolling() {
    bool any = false;
    for (const auto& g : cfg_.games) any = any || !g.onlineId.empty();
    if (!any || (cfg_.rooms.servers.empty() && cfg_.rooms.server.empty())) return;
    roomsWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HWND wnd = hwnd_;
    roomsThread_ = std::thread([this, wnd] {
        std::wstring server;
        while (!roomsStop_) {
            if (onlineOn_) {
                if (server.empty()) {
                    server = resolveRoomServer(cfg_.rooms.servers, cfg_.rooms.server, nullptr);
                    std::lock_guard<std::mutex> lock(installMu_);
                    roomServer_ = server;
                }
                std::map<std::string, std::vector<RoomInfo>> found;
                bool ok = false;
                int ping = -1;  // the fastest answer this round, in ms
                std::set<std::string> rejected;
                for (const auto& g : cfg_.games) {
                    if (g.onlineId.empty() || g.comingSoon || roomsStop_) continue;
                    std::vector<RoomInfo> list;
                    std::wstring err;
                    ULONGLONG asked = GetTickCount64();
                    bool refused = false;
                    bool listed = listRooms(server, g.onlineId, list, err, 4000, &refused);
                    if (refused) {
                        ok = true;  // the server answered; it just doesn't run this game
                        rejected.insert(g.id);
                    }
                    if (listed) {
                        int ms = static_cast<int>(GetTickCount64() - asked);
                        if (ping < 0 || ms < ping) ping = ms;
                        ok = true;
                        found[g.id] = std::move(list);
                    } else {
                        logger().debug(L"Rooms: " + err);
                    }
                }
                if (!ok) server.clear();  // ask mods-info again next time; the server may have moved
                {
                    std::lock_guard<std::mutex> lock(installMu_);
                    pendingRooms_ = std::move(found);
                    pendingRoomsOk_ = ok;
                    pendingPing_ = ping;
                    pendingRejected_ = std::move(rejected);
                }
                PostMessageW(wnd, WM_APP_ROOMS, 0, 0);
            }
            WaitForSingleObject(roomsWake_, static_cast<DWORD>(cfg_.rooms.refreshSeconds) * 1000);
        }
    });
}

void Launcher::onRooms() {
    std::map<std::string, std::vector<RoomInfo>> found;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        found.swap(pendingRooms_);
        ok = pendingRoomsOk_;
        pingMs_ = pendingPing_;
        if (pendingRejected_ != rejected_) {
            rejected_ = pendingRejected_;
            InvalidateRect(grid_, nullptr, FALSE);
            layoutDetail();
        }
    }
    serverAsked_ = true;
    InvalidateRect(hwnd_, &status_, FALSE);
    auto same = [](const std::vector<RoomInfo>& a, const std::vector<RoomInfo>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].code != b[i].code || a[i].players != b[i].players || a[i].name != b[i].name) return false;
        return true;
    };
    bool changed = ok != roomsKnown_ || found.size() != rooms_.size();
    for (const auto& [id, list] : found)
        if (!changed) changed = !rooms_.count(id) || !same(rooms_[id], list);
    if (!changed) return;  // nothing new: no repaint, no flicker
    rooms_ = std::move(found);
    roomsKnown_ = ok;
    refreshRoomsNav();
    layoutRoomsPage();
    InvalidateRect(hwnd_, &gridRc_, FALSE);
    if (motion_)
        for (const auto& [id, list] : rooms_)
            for (const auto& room : list) codeFrom_.try_emplace(room.code, GetTickCount64());
    InvalidateRect(grid_, nullptr, FALSE);
    layoutDetail();
}

void Launcher::joinRoom(size_t index) {
    if (index < tabRows_.size()) joinListedRoom(tabRows_[index].game, tabRows_[index].room);
}

std::vector<CatalogFile> Launcher::outdatedFor(const GameSpec& game) const {
    std::vector<CatalogFile> out;
    for (const auto& f : catalog_)
        if (f.game == game.id && outdated_.count(f.name)) out.push_back(f);
    return out;
}

void Launcher::checkOutdated() {
    outdated_.clear();
    for (const auto& f : outdatedGameFiles(catalog_, lib_.vars().get(L"GAMES_DIR"))) outdated_.insert(f.name);
}

void Launcher::copyAddress() {
    if (Session* s = webHostSession(selected_)) {
        const auto& links = !s->shareLinks.empty() ? s->shareLinks : s->lanLinks;
        if (!links.empty()) {
            copyText(links.front());
            toast(cfg_.t("notice.copied", {{L"ADDRESS", links.front()}}), theme_.c.success);
        }
        return;
    }
    std::wstring addr = (addresses_.empty() ? std::wstring(L"127.0.0.1") : addresses_.front().address) + L":" +
                        std::to_wstring(relay_.port());
    copyText(addr);
    toast(cfg_.t("notice.copied", {{L"ADDRESS", addr}}), theme_.c.success);
}

void Launcher::toggleFavorite() {
    if (selected_ < 0) return;
    GameStats& st = user_.stats(cfg_.games[selected_].id);
    st.favorite = !st.favorite;
    save();
    buildNav();
    if (navItems_[navIndex_].key == "fav") rebuildGrid();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
    InvalidateRect(nav_, nullptr, FALSE);
}

void Launcher::onPlayerChoice() {
    if (selected_ < 0) return;
    int sel = static_cast<int>(SendMessageW(playerCombo_, CB_GETCURSEL, 0, 0));
    auto* p = sel > 0 ? reinterpret_cast<const PlayerSpec*>(SendMessageW(playerCombo_, CB_GETITEMDATA, sel, 0)) : nullptr;
    user_.stats(cfg_.games[selected_].id).player = p ? p->id : std::string();
    save();
    select(selected_);
    InvalidateRect(grid_, nullptr, FALSE);
}

const PlayerSpec* Launcher::coopSavePlayer() const {
    return gameOptions().coopSavePlayer();
}

bool Launcher::coopSaveSupported() const {
    return gameOptions().coopSaveSupported();
}

bool Launcher::playerAtLeast(const std::wstring& version) const {
    return gameOptions().playerAtLeast(version);
}

const std::vector<Setting>& Launcher::settingsFor(const GameSpec& g) const {
    return gameOptions().settingsFor(g);
}

std::string Launcher::settingValue(const GameSpec& g, const Setting& s) const {
    return gameOptions().settingValue(g, s);
}

std::vector<std::wstring> Launcher::settingArgs(const GameSpec& g, const std::vector<std::wstring>& actionArgs) const {
    return gameOptions().settingArgs(g, actionArgs);
}

void Launcher::onSettingChanged(int index, bool combo) {
    if (selected_ < 0) return;
    const GameSpec& g = cfg_.games[selected_];
    const std::vector<Setting>& list = settingsFor(g);
    if (index < 0 || static_cast<size_t>(index) >= list.size()) return;
    const Setting& s = list[index];
    std::string value;
    if (combo) {
        int sel = static_cast<int>(SendMessageW(settingCombos_[index], CB_GETCURSEL, 0, 0));
        if (sel < 0 || static_cast<size_t>(sel) >= s.choices.size()) return;
        value = s.choices[sel].id;
    } else {
        value = SendMessageW(settingChecks_[index], BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0";
    }
    user_.stats(g.id).settings[s.id] = value;
    save();
}

std::wstring Launcher::coopSaveFile(const GameSpec& g) const {
    return gameOptions().coopSaveFile(g);
}

int Launcher::coopSaveChoiceFor(const GameSpec& g) const {
    auto it = coopSaveChoice_.find(g.id);
    return it == coopSaveChoice_.end() ? 0 : it->second;
}

void Launcher::onSaveChoice() {
    if (selected_ < 0) return;
    int sel = static_cast<int>(SendMessageW(saveCombo_, CB_GETCURSEL, 0, 0));
    coopSaveChoice_[cfg_.games[selected_].id] = std::clamp(sel, 0, 2);
    layoutDetail();
    InvalidateRect(hwnd_, &detail_, FALSE);
}

void Launcher::save() {
    if (snapshot_) return;
    std::wstring err;
    if (!saveUserData(user_, userPath_, &err)) logger().warn(L"Could not save settings: " + err);
}

std::wstring Launcher::formatDuration(int64_t s) const {
    if (s < 60) return std::to_wstring(s) + L" s";
    if (s < 3600) return std::to_wstring(s / 60) + L" min";
    return std::to_wstring(s / 3600) + L" h " + std::to_wstring((s % 3600) / 60) + L" min";
}

std::wstring Launcher::formatAgo(int64_t t) const {
    if (t <= 0) return cfg_.t("time.never");
    int64_t d = unixNow() - t;
    if (d < 60) return cfg_.t("time.justNow");
    if (d < 3600) return cfg_.t("time.minutesAgo", {{L"N", std::to_wstring(d / 60)}});
    if (d < 86400) return cfg_.t("time.hoursAgo", {{L"N", std::to_wstring(d / 3600)}});
    return cfg_.t("time.daysAgo", {{L"N", std::to_wstring(d / 86400)}});
}

}  // namespace fl::ui
