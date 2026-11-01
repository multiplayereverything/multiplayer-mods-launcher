// The launcher window: construction, window setup and filling the controls from the data.
#include "launcher.h"

#include <algorithm>
#include <cwctype>
#include <regex>

#include "core/embedded.h"
#include "core/fs.h"
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

Launcher::Launcher(const Config& cfg, Library& lib, UserData& user, std::wstring userPath)
    : cfg_(cfg), lib_(lib), user_(user), userPath_(std::move(userPath)) {}

Launcher::~Launcher() {
    closeLobby();
    if (lobbyNetThread_.joinable()) lobbyNetThread_.join();
    if (siteThread_.joinable()) siteThread_.join();
    installCancel_ = true;
    if (installThread_.joinable()) installThread_.join();
    if (catalogThread_.joinable()) catalogThread_.join();
    if (updateThread_.joinable()) updateThread_.join();
    modCancel_.cancel();
    if (modsThread_.joinable()) modsThread_.join();
    for (auto& s : sessions_)
        if (s.server) s.server->stop();  // wakes the start-up waits below
    for (auto& t : helpers_)
        if (t.joinable()) t.join();
    roomsStop_ = true;
    if (roomsWake_) SetEvent(roomsWake_);
    if (roomsThread_.joinable()) roomsThread_.join();
    if (roomsWake_) CloseHandle(roomsWake_);
    relay_.stop();
    watcher_.reset();
}

// ---- window setup --------------------------------------------------------

int Launcher::run(const LauncherOptions& opt) {
    snapshot_ = !opt.snapshot.empty();
    create(opt);
    if (!hwnd_) return 1;
    if (!opt.select.empty())
        for (size_t i = 0; i < cfg_.games.size(); ++i)
            if (cfg_.games[i].id == opt.select) select(static_cast<int>(i));
    if (opt.demoHost && selected_ >= 0) {
        std::wstring err;
        for (const auto& a : cfg_.games[selected_].actions)
            if (a.relay.enabled && relay_.start(a.relay.port, err)) {
                relayGame_ = selected_;
                addresses_ = sys::localAddresses();
                break;
            }
        layoutDetail();
    }

    if (snapshot_ && !cfg_.downloads.site.empty() && !getenv("FL_NO_SITE")) {
        std::wstring err;
        loadSite(cfg_.downloads.site, fs::join(lib_.dataDir(), L"site.json"), site_, err);
        fetchSiteCovers(cfg_, site_, cfg_.downloads.site, fs::join(lib_.dataDir(), L"covers"));
        layoutDetail();
    }
    if (snapshot_ && user_.online) {
        // Snapshots have no worker threads: fetch the room list once, directly.
        std::wstring server = resolveRoomServer(cfg_.rooms.servers, cfg_.rooms.server, nullptr);
        bool ok = false;
        std::map<std::string, std::vector<RoomInfo>> found;
        std::set<std::string> rejected;
        for (const auto& g : cfg_.games) {
            std::vector<RoomInfo> list;
            std::wstring err;
            bool refused = false;
            if (g.onlineId.empty() || g.comingSoon || server.empty()) continue;
            if (listRooms(server, g.onlineId, list, err, 2000, &refused)) {
                ok = true;
                found[g.id] = std::move(list);
            } else if (refused) {
                ok = true;
                rejected.insert(g.id);
            }
        }
        {
            std::lock_guard<std::mutex> lock(installMu_);
            pendingRooms_ = std::move(found);
            pendingRoomsOk_ = ok;
            pendingRejected_ = std::move(rejected);
        }
        onRooms();
    }
    if (opt.guide && selected_ >= 0) {  // after the website's list, which the page uses
        openGuide(selected_);
        scrollGuide(opt.guideScroll);
    }
    std::vector<std::unique_ptr<lobby::Session>> demoGuests;
    if (snapshot_ && !opt.lobby.empty() && selected_ >= 0 && usesLobby(selected_)) {
        openLobby(selected_);
        if (opt.lobby == "room") {
            // A direct room with two friends in it, the way it looks while you wait.
            if (user_.playerName.empty()) user_.playerName = L"Hana";
            SendMessageW(lbDirect_, BM_SETCHECK, BST_CHECKED, 0);
            SetWindowTextW(lbPort_, L"47991");
            lobbyHost();
            lobbyNetText_ = cfg_.t("lobby.portOpen", {{L"PORT", L"47991"}});
            const wchar_t* names[] = {L"Ann", L"Bob"};
            for (const wchar_t* n : names) {
                lobby::Options o;
                o.transport = lobby::Transport::Direct;
                o.server = L"127.0.0.1:47991";
                o.gameId = cfg_.games[selected_].onlineId;
                o.version = lobbyVersion(selected_);
                o.playerName = n;
                auto s = std::make_unique<lobby::Session>();
                std::wstring err;
                if (s->start(o, err)) demoGuests.push_back(std::move(s));
                Sleep(300);
            }
            if (!demoGuests.empty()) {
                demoGuests[0]->setReady(true);
                demoGuests[0]->sendChat(L"ready when you are");
            }
            for (int i = 0; i < 40; ++i) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(25);
            }
        }
    }
    if (snapshot_) {
        auto pump = [] {
            for (int i = 0; i < 15; ++i) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                Sleep(20);
            }
        };
        pump();
        HWND target = lobbyWnd_ ? lobbyWnd_ : hwnd_;
        RECT cr;
        GetClientRect(target, &cr);
        HDC screen = GetDC(target);
        Buffer buf(screen, cr.right, cr.bottom);
        ReleaseDC(target, screen);
        PrintWindow(target, buf.dc, PW_CLIENTONLY | PW_RENDERFULLCONTENT);
        SelectObject(buf.dc, buf.old);  // GDI+ must not read a bitmap still selected into a DC
        CLSID png;
        int code = 1;
        if (pngEncoder(png)) {
            Bitmap bmp(buf.bmp, nullptr);
            fs::ensureDir(fs::parent(opt.snapshot), nullptr, nullptr);
            code = bmp.Save(opt.snapshot.c_str(), &png, nullptr) == Ok ? 0 : 1;
        }
        buf.old = SelectObject(buf.dc, buf.bmp);
        relay_.stop();
        demoGuests.clear();
        if (lobbyWnd_) DestroyWindow(lobbyWnd_);
        DestroyWindow(hwnd_);
        return code;
    }

    // Everyone online needs a name; ask once, on the very first start.
    if (user_.playerName.empty() && !getenv("FL_NO_NAME_PROMPT")) askName(true);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (ctrl && msg.wParam == 'F') {
                SetFocus(search_);
                SendMessageW(search_, EM_SETSEL, 0, -1);
                continue;
            }
            if (ctrl && msg.wParam == VK_OEM_COMMA) {
                openSettings();
                continue;
            }
            if (guidePage() && msg.hwnd != search_ && msg.hwnd != tabCodeEdit_ && msg.hwnd != pageCodeEdit_) {
                int page = std::max(theme_.s(48), height(guideView_) - theme_.s(48));
                switch (msg.wParam) {
                    case VK_ESCAPE: closeGuide(); continue;
                    case VK_NEXT: scrollGuide(page); continue;
                    case VK_PRIOR: scrollGuide(-page); continue;
                    case VK_DOWN: scrollGuide(theme_.s(48)); continue;
                    case VK_UP: scrollGuide(-theme_.s(48)); continue;
                    case VK_HOME: scrollGuide(-guideContentH_); continue;
                    case VK_END: scrollGuide(guideContentH_); continue;
                    default: break;
                }
            }
            if (msg.wParam == VK_F5) {
                forceModsFetch_ = true;
                refreshAll();
                continue;
            }
            if (msg.wParam == VK_ESCAPE && msg.hwnd == search_ && GetWindowTextLengthW(search_)) {
                SetWindowTextW(search_, L"");
                continue;
            }
            if (msg.wParam == VK_RETURN && (msg.hwnd == tabCodeEdit_ || msg.hwnd == pageCodeEdit_)) {
                if (msg.hwnd == tabCodeEdit_) joinByCode(tabCodeEdit_, selected_);
                else joinByCode(pageCodeEdit_, -1);
                continue;
            }
            // Enter on a card opens its page, where its buttons are (the library itself is only the cards).
            if (msg.wParam == VK_RETURN && msg.hwnd == grid_) {
                int item = ListView_GetNextItem(grid_, -1, LVNI_SELECTED);
                if (item >= 0 && item < static_cast<int>(visible_.size())) openGuide(visible_[item]);
                else if (selected_ >= 0) openGuide(selected_);
                continue;
            }
            if (msg.wParam == VK_RETURN && msg.hwnd == search_) {
                if (!visible_.empty()) {
                    SetFocus(grid_);
                    ListView_SetItemState(grid_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                }
                continue;
            }
        }
        if (lobbyWnd_ && IsChild(lobbyWnd_, msg.hwnd) && IsDialogMessageW(lobbyWnd_, &msg)) continue;
        if (IsDialogMessageW(hwnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void Launcher::create(const LauncherOptions& opt) {
    motion_ = !snapshot_ && systemAnimations();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    theme_.init(cfg_);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = theme_.iconBig;
    wc.hIconSm = theme_.iconSmall;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    watcher_ = std::make_unique<sys::ProcessWatcher>([this](DWORD pid, DWORD code) {
        if (hwnd_) PostMessageW(hwnd_, WM_APP_EXIT, pid, static_cast<LPARAM>(code));
    });
    relay_.setOnChange([this] {
        if (hwnd_) PostMessageW(hwnd_, WM_APP_RELAY, 0, 0);
    });

    // Create at 96 DPI units, then resize once the real monitor DPI is known.
    UINT dpi = GetDpiForSystem();
    int w = opt.width ? opt.width : MulDiv(cfg_.branding.window.width, dpi, 96);
    int h = opt.height ? opt.height : MulDiv(cfg_.branding.window.height, dpi, 96);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (!snapshot_ && user_.width > 0 && user_.height > 0) {
        RECT want{user_.x, user_.y, user_.x + user_.width, user_.y + user_.height};
        if (MonitorFromRect(&want, MONITOR_DEFAULTTONULL)) {  // still on a connected screen
            x = user_.x;
            y = user_.y;
            w = user_.width;
            h = user_.height;
        }
    }
    Vars v = lib_.vars();
    std::wstring title = v.expand(cfg_.branding.windowTitle);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), style, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return;
    theme_.build(GetDpiForWindow(hwnd_));
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    createControls();
    refreshMods();
    buildNav();
    rebuildGrid();
    int start = -1;
    for (size_t i = 0; i < cfg_.games.size(); ++i)
        if (cfg_.games[i].id == user_.selectedGame) start = static_cast<int>(i);
    if (start < 0 && !visible_.empty()) start = visible_.front();
    select(start);
    relayout();
    // The cards were added before the grid knew its tile size; added again now,
    // they land in order (otherwise the first arrange can leave them shuffled).
    rebuildGrid();
    if (snapshot_) {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(hwnd_, user_.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        if (motion_) startIntro();
        SetTimer(hwnd_, kTimer, 1000, nullptr);
        // Nothing can start without a player: offer to install one right away.
        bool anyPlayer = false, installable = false;
        for (const auto& p : lib_.players()) {
            anyPlayer = anyPlayer || p.found();
            installable = installable || p.spec->install.enabled();
        }
        if (!anyPlayer && installable) PostMessageW(hwnd_, WM_APP_FIRSTRUN, 0, 0);
        onlineOn_ = user_.online;
        startRoomPolling();
    if (!snapshot_) startSiteFetch();
        startModsFetch();
        if (!cfg_.discord.clientId.empty()) discord_ = std::make_unique<DiscordPresence>(cfg_.discord.clientId);
        startUpdateCheck();
    }
    loadCatalog();
    SetFocus(intro_ ? hwnd_ : grid_);  // the grid is hidden while the entrance plays
}

void Launcher::createControls() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    auto child = [&](const wchar_t* cls, DWORD style, DWORD ex, int id) {
        // WS_CLIPSIBLINGS: the toast floats above the other controls.
        return CreateWindowExW(ex, cls, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    nav_ = child(L"LISTBOX", WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 0,
                 kIdNav);
    navCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdNavCombo);
    sortCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdSort);
    search_ = child(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL, 0, kIdSearch);
    SetWindowSubclass(search_, searchProc, 3, reinterpret_cast<DWORD_PTR>(this));
    grid_ = child(WC_LISTVIEWW, WS_TABSTOP | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_AUTOARRANGE | LVS_NOCOLUMNHEADER, 0,
                  kIdGrid);
    ListView_SetExtendedListViewStyle(grid_, LVS_EX_DOUBLEBUFFER);
    ListView_SetView(grid_, LV_VIEW_TILE);
    LVCOLUMNW col{};
    col.mask = LVCF_WIDTH;
    col.cx = 100;
    ListView_InsertColumn(grid_, 0, &col);
    SetWindowSubclass(grid_, gridProc, 1, reinterpret_cast<DWORD_PTR>(this));
    playerCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdPlayer);
    saveCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdSave);
    for (size_t i = 0; i < settingChecks_.size(); ++i) {
        settingChecks_[i] = makeButton(kIdSetting0 + static_cast<int>(i), BtnStyle::Check);
        settingCombos_[i] = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdSettingCombo0 + static_cast<int>(i));
    }
    for (const char* key : {"coopSave.continue", "coopSave.solo", "coopSave.new"})
        SendMessageW(saveCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg_.t(key).c_str()));
    favBtn_ = makeButton(kIdFav, BtnStyle::Secondary);
    folderBtn_ = makeButton(kIdFolder, BtnStyle::Secondary);
    aboutBtn_ = makeButton(kIdAbout, BtnStyle::Secondary);
    heroBtn_ = makeButton(kIdHero, BtnStyle::Secondary);
    sizeBtn_ = makeButton(kIdSize, BtnStyle::Secondary);
    libTab_ = makeButton(kIdTabLibrary, BtnStyle::Tab);
    roomsTopTab_ = makeButton(kIdTabRoomsTop, BtnStyle::Tab);
    btnInfo_[sizeBtn_].icon = kIconCards;
    catBtn_ = makeButton(kIdCatChip, BtnStyle::Secondary);
    sortBtn_ = makeButton(kIdSortChip, BtnStyle::Secondary);
    btnInfo_[catBtn_].menu = btnInfo_[sortBtn_].menu = true;
    heroPlayBtn_ = makeButton(kIdHeroPlay, BtnStyle::Primary);
    for (int i = 0; i < 2; ++i) {
        pageTabs_[i] = makeButton(kIdPageTab0 + i, BtnStyle::Tab);
        SetWindowTextW(pageTabs_[i], cfg_.t(i == 0 ? "page.overview" : "page.details").c_str());
    }
    guideBack_ = makeButton(kIdGuideBack, BtnStyle::Secondary);
    for (int i = 0; i < static_cast<int>(kGuideMaxLinks); ++i) guideLinkBtns_.push_back(makeButton(kIdGuideLink0 + i, BtnStyle::Secondary));
    locateBtn_ = makeButton(kIdLocate, BtnStyle::Secondary);
    getPlayerBtn_ = makeButton(kIdGetPlayer, BtnStyle::Secondary);
    copyBtn_ = makeButton(kIdCopy, BtnStyle::Secondary);
    stopHostBtn_ = makeButton(kIdStopHost, BtnStyle::Danger);
    downloadBtn_ = makeButton(kIdDownload, BtnStyle::Secondary);
    settingsBtn_ = makeButton(kIdSettings, BtnStyle::Ghost);
    SetWindowTextW(settingsBtn_, cfg_.t("button.settings").c_str());
    btnInfo_[settingsBtn_].icon = kIconSettings;
    ShowWindow(settingsBtn_, SW_SHOW);

    // Details panel: Play / Rooms tabs, Quick play, "Other ways to connect", join by code.
    tabPlayBtn_ = makeButton(kIdTabPlay, BtnStyle::Tab);
    tabRoomsBtn_ = makeButton(kIdTabRooms, BtnStyle::Tab);
    quickBtn_ = makeButton(kIdQuick, BtnStyle::Secondary);
    btnInfo_[quickBtn_].icon = kIconQuick;
    moreBtn_ = makeButton(kIdMore, BtnStyle::Ghost);
    hostGameBtn_ = makeButton(kIdHostGame, BtnStyle::Primary);
    joinGameBtn_ = makeButton(kIdJoinGame, BtnStyle::Secondary);
    SetWindowTextW(hostGameBtn_, cfg_.t("button.hostGame").c_str());
    SetWindowTextW(joinGameBtn_, cfg_.t("button.joinGame").c_str());
    btnInfo_[hostGameBtn_].icon = L"\uE774";
    btnInfo_[joinGameBtn_].icon = L"\uE774";
    tabCodeEdit_ = child(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE, 0, kIdTabCode);
    tabCodeBtn_ = makeButton(kIdTabCodeJoin, BtnStyle::Secondary);
    // The Rooms page: game filter, hide full rooms, join by code.
    pageGameCombo_ = child(WC_COMBOBOXW, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, kIdPageGame);
    pageHideFull_ = makeButton(kIdPageHideFull, BtnStyle::Check);
    SetWindowTextW(pageHideFull_, cfg_.t("roomsPage.hideFull").c_str());
    pageCodeEdit_ = child(L"EDIT", WS_TABSTOP | ES_AUTOHSCROLL | ES_UPPERCASE, 0, kIdPageCode);
    pageCodeBtn_ = makeButton(kIdPageCodeJoin, BtnStyle::Secondary);
    for (HWND e : {tabCodeEdit_, pageCodeEdit_}) {
        SendMessageW(e, EM_LIMITTEXT, 8, 0);
        SetWindowSubclass(e, searchProc, 3, reinterpret_cast<DWORD_PTR>(this));  // draws the "Room code" hint
        SetWindowTextW(e, L"");
        ShowWindow(e, SW_HIDE);
    }
    for (HWND b : {tabCodeBtn_, pageCodeBtn_}) SetWindowTextW(b, cfg_.t("roomsPage.joinCode").c_str());
    std::wstring all = cfg_.t("roomsPage.allGames");
    SendMessageW(pageGameCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(all.c_str()));
    SendMessageW(pageGameCombo_, CB_SETITEMDATA, 0, static_cast<LPARAM>(-1));
    for (size_t i = 0; i < cfg_.games.size(); ++i) {
        if (cfg_.games[i].onlineId.empty() || cfg_.games[i].comingSoon) continue;
        int at = static_cast<int>(
            SendMessageW(pageGameCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg_.games[i].name.c_str())));
        SendMessageW(pageGameCombo_, CB_SETITEMDATA, at, static_cast<LPARAM>(i));
    }
    SendMessageW(pageGameCombo_, CB_SETCURSEL, 0, 0);
    ShowWindow(pageGameCombo_, SW_HIDE);

    // Toasts: a small layered child window over the game grid.
    static bool toastClass = false;
    if (!toastClass) {
        WNDCLASSEXW tc{sizeof tc};
        tc.lpfnWndProc = toastProc;
        tc.hInstance = GetModuleHandleW(nullptr);
        tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        tc.lpszClassName = kToastClass;
        RegisterClassExW(&tc);
        toastClass = true;
    }
    toast_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kToastClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10,
                             hwnd_, nullptr, GetModuleHandleW(nullptr), this);
    if (theme_.dark && !theme_.highContrast) {
        for (HWND h : {nav_, grid_}) SetWindowTheme(h, L"DarkMode_Explorer", nullptr);
        for (HWND h : {navCombo_, sortCombo_, playerCombo_, saveCombo_, pageGameCombo_}) SetWindowTheme(h, L"DarkMode_CFD", nullptr);
        for (HWND h : settingCombos_) SetWindowTheme(h, L"DarkMode_CFD", nullptr);
        // Dark popup menus for the toolbar's lists. uxtheme exports this only by ordinal (135, then 136 to apply it);
        // where it is missing the menus stay light, which still works.
        if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            using SetMode = int(WINAPI*)(int);
            using Flush = void(WINAPI*)();
            if (auto set = reinterpret_cast<SetMode>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(135))))) set(2);  // force dark
            if (auto flush = reinterpret_cast<Flush>(reinterpret_cast<void*>(GetProcAddress(ux, MAKEINTRESOURCEA(136))))) flush();
        }
    }
    applyFonts();
}

HWND Launcher::makeButton(int id, BtnStyle style, HWND parent) {
    // A real push button painted in NM_CUSTOMDRAW (not BS_OWNERDRAW) keeps the
    // Button role and Invoke action that screen readers and UI Automation use.
    DWORD kind = style == BtnStyle::Check ? BS_AUTOCHECKBOX : BS_PUSHBUTTON;
    HWND b = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS | kind, 0, 0, 10, 10, parent ? parent : hwnd_,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SetWindowSubclass(b, buttonProc, 2, reinterpret_cast<DWORD_PTR>(this));
    btnInfo_[b].style = style;
    SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.strong), FALSE);
    return b;
}

void Launcher::applyFonts() {
    for (HWND h : {nav_, navCombo_, sortCombo_, search_, grid_, playerCombo_, saveCombo_, pageGameCombo_})
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
    for (HWND h : settingCombos_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), TRUE);
    for (HWND h : {tabCodeEdit_, pageCodeEdit_})
        if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.button), TRUE);
    for (auto& [b, info] : btnInfo_) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.strong), FALSE);
    SendMessageW(nav_, LB_SETITEMHEIGHT, 0, theme_.s(38));
    SendMessageW(search_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(theme_.s(4), theme_.s(4)));
    ListView_SetBkColor(grid_, theme_.c.background.ref());
}

// ---- data -> controls ----------------------------------------------------

void Launcher::buildNav() {
    navItems_.clear();
    // The platforms (PC mods, Flash mods) lead the sidebar; the other categories follow the views.
    auto addCategories = [&](bool platform) {
        for (const auto& c : cfg_.categories)
            if ((c.group == "platform") == platform)
                navItems_.push_back({"cat:" + c.id, c.name, c.icon, c.group.empty() ? "categories" : c.group});
    };
    addCategories(true);
    // A launcher.json without platform categories has nothing to lead with, so it keeps the plain list of games.
    if (navItems_.empty()) navItems_.push_back({"all", cfg_.t("nav.all"), kIconAll});
    navItems_.push_back({"fav", cfg_.t("nav.favorites"), kIconFav});
    navItems_.push_back({"ready", cfg_.t("nav.ready"), kIconReady});
    bool online = std::any_of(cfg_.games.begin(), cfg_.games.end(), [](const GameSpec& g) { return !g.onlineId.empty(); });
    if (online) navItems_.push_back({"rooms", cfg_.t("nav.rooms"), kIconRooms});
    if (std::any_of(cfg_.games.begin(), cfg_.games.end(), [](const GameSpec& g) { return g.comingSoon; }))
        navItems_.push_back({"soon", cfg_.t("nav.soon"), kIconSoon});
    addCategories(false);
    for (auto& n : navItems_) {
        n.count = 0;
        if (n.key == "rooms") {
            n.count = playersOnline();
            continue;
        }
        for (size_t i = 0; i < cfg_.games.size(); ++i) {
            const auto& g = cfg_.games[i];
            const GameStats* st = user_.find(g.id);
            // Games that aren't released yet get a view of their own; the other lists show what you can use.
            bool in = (n.key == "all" && !g.comingSoon) || (n.key == "soon" && g.comingSoon) ||
                      (n.key == "fav" && st && st->favorite) || (n.key == "ready" && gameReady(static_cast<int>(i))) ||
                      (n.key.rfind("cat:", 0) == 0 && !g.comingSoon &&
                       std::find(g.categories.begin(), g.categories.end(), n.key.substr(4)) != g.categories.end());
            if (in) ++n.count;
        }
    }
    navIndex_ = 0;
    for (size_t i = 0; i < navItems_.size(); ++i)
        if (navItems_[i].key == user_.category) navIndex_ = i;
    SendMessageW(nav_, LB_RESETCONTENT, 0, 0);
    SendMessageW(navCombo_, CB_RESETCONTENT, 0, 0);
    for (const auto& n : navItems_) {
        // The visible count is drawn separately; screen readers get it in the text.
        std::wstring text = n.label + L" (" +
                            (n.key == "rooms" ? cfg_.t("nav.online", {{L"N", std::to_wstring(n.count)}})
                                              : std::to_wstring(n.count)) +
                            L")";
        SendMessageW(nav_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(navCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
    SendMessageW(navCombo_, CB_SETCURSEL, navIndex_, 0);
    SendMessageW(sortCombo_, CB_RESETCONTENT, 0, 0);
    for (const char* key : {"sort.ours", "sort.recent", "sort.name", "sort.players"})
        SendMessageW(sortCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(cfg_.t(key).c_str()));
    SendMessageW(sortCombo_, CB_SETCURSEL, user_.sortMode, 0);
    SetWindowTextW(sizeBtn_, cfg_.t(user_.compactCards ? "button.normalCards" : "button.compactCards").c_str());
    SetWindowTextW(libTab_, cfg_.t("tab.library").c_str());
    SetWindowTextW(roomsTopTab_, cfg_.t("tab.rooms").c_str());
    syncTopTabs();
}

// The top bar's tabs follow the view: Rooms is its own page, everything else is the library.
void Launcher::syncTopTabs() {
    bool rooms = roomsPage();
    if (!rooms && !navItems_.empty()) libraryKey_ = navItems_[navIndex_].key;
    if (btnInfo_[libTab_].selected != !rooms || btnInfo_[roomsTopTab_].selected != rooms) {
        btnInfo_[libTab_].selected = !rooms;
        btnInfo_[roomsTopTab_].selected = rooms;
        InvalidateRect(libTab_, nullptr, FALSE);
        InvalidateRect(roomsTopTab_, nullptr, FALSE);
    }
    syncChips();
}

// Each chip says the choice its combo holds.
void Launcher::syncChips() {
    bool changed = false;
    for (auto [chip, combo] : {std::pair{catBtn_, navCombo_}, std::pair{sortBtn_, sortCombo_}}) {
        int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
        int len = sel >= 0 ? static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, sel, 0)) : 0;
        std::wstring text(static_cast<size_t>(std::max(len, 0)) + 1, L'\0');
        if (len > 0) SendMessageW(combo, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(std::max(len, 0)));
        if (windowText(chip) != text) {
            SetWindowTextW(chip, text.c_str());
            InvalidateRect(chip, nullptr, FALSE);
            changed = true;
        }
    }
    if (changed && !intro_) relayout();  // a chip is as wide as its text
}

// The combo's items as a menu under the chip; a choice goes through the combo's own CBN_SELCHANGE handling.
void Launcher::dropList(HWND chip, HWND combo, int comboId) {
    HMENU menu = CreatePopupMenu();
    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    for (int i = 0; i < count; ++i) {
        int len = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, i, 0));
        std::wstring text(static_cast<size_t>(std::max(len, 0)) + 1, L'\0');
        SendMessageW(combo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(text.data()));
        AppendMenuW(menu, MF_STRING | (i == sel ? MF_CHECKED : 0), static_cast<UINT_PTR>(i + 1), text.c_str());
    }
    RECT r;
    GetWindowRect(chip, &r);
    int pick = static_cast<int>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                                               r.left, r.bottom + theme_.s(4), 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (pick <= 0 || pick - 1 == sel) return;
    SendMessageW(combo, CB_SETCURSEL, pick - 1, 0);
    SendMessageW(hwnd_, WM_COMMAND, MAKEWPARAM(comboId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    syncChips();
}

void Launcher::openNav(const std::string& key) {
    for (size_t i = 0; i < navItems_.size(); ++i) {
        if (navItems_[i].key != key) continue;
        navIndex_ = i;
        SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
        SendMessageW(navCombo_, CB_SETCURSEL, navIndex_, 0);
        user_.category = key;
        closeGuide();
        rebuildGrid();
        return;
    }
}

bool Launcher::matches(int gi) const {
    const GameSpec& g = cfg_.games[gi];
    const std::string& key = navItems_.empty() ? std::string("all") : navItems_[navIndex_].key;
    const GameStats* st = user_.find(g.id);
    if (key == "rooms") return false;  // the Rooms page replaces the grid
    bool searching = !trim(windowText(search_)).empty();
    if (key == "fav" && !(st && st->favorite)) return false;
    if (key == "ready" && !gameReady(gi)) return false;
    if (key == "soon" && !g.comingSoon) return false;
    // Coming soon games show only in their own view, or when you search for one by name.
    if (g.comingSoon && key != "soon" && key != "fav" && !searching) return false;
    if (key.rfind("cat:", 0) == 0 &&
        std::find(g.categories.begin(), g.categories.end(), key.substr(4)) == g.categories.end())
        return false;
    std::wstring hay = toLower(g.name + L" " + displayName(g) + L" " + g.description + L" " + g.playerCount);
    for (const auto& c : g.categories)
        if (auto cat = cfg_.findCategory(c)) hay += L" " + toLower(cat->name);
    for (const auto& word : split(toLower(trim(windowText(search_))), L' '))
        if (!word.empty() && hay.find(word) == std::wstring::npos) return false;
    return true;
}

int Launcher::gridRank(int gi) const {
    const GameSpec& g = cfg_.games[gi];
    const GameStats* st = user_.find(g.id);
    if (g.comingSoon) return 100;
    // PC mods first, then Flash games; inside each, favorites, then what is ready, then the rest.
    int platform = g.mod ? 0 : 10;
    if (st && st->favorite) return platform;
    return platform + (gameReady(gi) ? 1 : 2);
}

void Launcher::rebuildGrid() {
    visible_.clear();
    for (size_t i = 0; i < cfg_.games.size(); ++i)
        if (matches(static_cast<int>(i))) visible_.push_back(static_cast<int>(i));
    std::stable_sort(visible_.begin(), visible_.end(), [this](int a, int b) {
        const GameStats *sa = user_.find(cfg_.games[a].id), *sb = user_.find(cfg_.games[b].id);
        switch (user_.sortMode) {
            case 1: {  // what was played last first; games never played keep our order after them
                int64_t ta = sa ? sa->lastPlayed : 0, tb = sb ? sb->lastPlayed : 0;
                if (ta != tb) return ta > tb;
                break;
            }
            case 2:
                return CompareStringOrdinal(displayName(cfg_.games[a]).c_str(), -1, displayName(cfg_.games[b]).c_str(), -1,
                                            TRUE) == CSTR_LESS_THAN;
            case 3:
                if (cfg_.games[a].maxPlayers != cfg_.games[b].maxPlayers) return cfg_.games[a].maxPlayers > cfg_.games[b].maxPlayers;
                break;
            default: break;
        }
        return gridRank(a) < gridRank(b);
    });
    suppressSelect_ = true;
    SendMessageW(grid_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(grid_);
    int selIndex = -1;
    for (size_t i = 0; i < visible_.size(); ++i) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = static_cast<int>(i);
        std::wstring name = displayName(cfg_.games[visible_[i]]);
        it.pszText = name.data();
        it.lParam = visible_[i];
        ListView_InsertItem(grid_, &it);
        if (visible_[i] == selected_) selIndex = static_cast<int>(i);
    }
    if (selIndex >= 0) {
        ListView_SetItemState(grid_, selIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(grid_, selIndex, FALSE);
    }
    SendMessageW(grid_, WM_SETREDRAW, TRUE, 0);
    suppressSelect_ = false;
    ShowWindow(grid_, visible_.empty() || roomsPage() || guidePage() ? SW_HIDE : SW_SHOW);
    // The banner comes and goes with the page, the search and the last game played.
    syncTopTabs();
    if (!intro_ && (heroWanted() ? heroCandidate() : -1) != heroGame_) relayout();
    hoverItem_ = -1;
    pageScroll_ = 0;
    layoutRoomsPage();
    InvalidateRect(grid_, nullptr, TRUE);
    InvalidateRect(hwnd_, &gridRc_, FALSE);
}

void Launcher::select(int gi) {
    if (guidePage() && gi != guideGame_) closeGuide();
    if (gi != selected_ && gi >= 0 && motion_) titleFrom_ = GetTickCount64();
    selected_ = gi;
    if (gi >= 0) user_.selectedGame = cfg_.games[gi].id;
    // Player choice: "Automatic" plus every player any action of this game allows.
    SendMessageW(playerCombo_, CB_RESETCONTENT, 0, 0);
    if (gi >= 0 && !cfg_.games[gi].mod) {  // a PC mod has no Flash player to choose
        const GameSpec& g = cfg_.games[gi];
        // Say what "Automatic" picks right now, so nobody has to guess.
        UserData noChoice = user_;
        noChoice.stats(g.id).player.clear();
        ResolvedAction autoPick = g.actions.empty() ? ResolvedAction{} : lib_.resolve(g, g.actions.front(), noChoice);
        std::wstring autoText = autoPick.state == ActionState::Ready && autoPick.player
                                    ? cfg_.t("detail.playerAuto", {{L"PLAYER", autoPick.player->name}})
                                    : cfg_.t("detail.playerAutoNone");
        SendMessageW(playerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(autoText.c_str()));
        const GameStats* st = user_.find(g.id);
        int sel = 0, idx = 1;
        for (const auto& p : cfg_.players) {
            bool allowed = false;
            for (const auto& a : g.actions)
                allowed = allowed || a.players.empty() ||
                          std::find(a.players.begin(), a.players.end(), p.id) != a.players.end();
            if (!allowed) continue;
            const PlayerStatus* ps = lib_.player(p.id);
            std::wstring text = p.name + (ps && ps->found() ? L"" : L"  (not found)");
            int at = static_cast<int>(SendMessageW(playerCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())));
            SendMessageW(playerCombo_, CB_SETITEMDATA, at, reinterpret_cast<LPARAM>(&p));
            if (st && st->player == p.id) sel = idx;
            ++idx;
        }
        SendMessageW(playerCombo_, CB_SETCURSEL, sel, 0);
    }
    if (gi < 0 || cfg_.games[gi].onlineId.empty()) detailTab_ = 0;
    // One button per action, created on demand and reused.
    size_t count = gi >= 0 ? cfg_.games[gi].actions.size() : 0;
    size_t lead = count;
    for (size_t i = 0; i < count && lead == count; ++i)
        if (actionVisible(gi, cfg_.games[gi].actions[i]) && !cfg_.games[gi].actions[i].more) lead = i;
    while (actionBtns_.size() < count)
        actionBtns_.push_back(makeButton(kIdAction0 + static_cast<int>(actionBtns_.size()), BtnStyle::Secondary));
    for (size_t i = 0; i < actionBtns_.size(); ++i) {
        HWND b = actionBtns_[i];
        if (i >= count) {
            ShowWindow(b, SW_HIDE);
            continue;
        }
        const GameSpec& g = cfg_.games[gi];
        const ActionSpec& a = g.actions[i];
        if (a.isMod()) {
            applyModAction(gi, a, b);
            continue;
        }
        ResolvedAction r = lib_.resolve(g, a, user_);
        BtnInfo& info = btnInfo_[b];
        info.style = i == lead ? BtnStyle::Primary : BtnStyle::Secondary;
        info.ready = r.state == ActionState::Ready;
        info.tag = a.original ? cfg_.t("badge.original") : std::wstring();
        info.icon = a.icon;
        std::wstring sub = a.description;
        if (sub.empty() && r.player) sub = r.player->name;
        if (r.state != ActionState::Ready) sub = stateReason(cfg_, r);
        info.sub = sub;
        SetWindowTextW(b, a.label.c_str());
    }
    layoutDetail();
    InvalidateRect(hwnd_, &detail_, FALSE);
}

int Launcher::selectedAction(int preferred) const {
    if (selected_ < 0) return -1;
    const auto& actions = cfg_.games[selected_].actions;
    if (preferred >= 0 && preferred < static_cast<int>(actions.size())) return preferred;
    return actions.empty() ? -1 : 0;
}

void Launcher::refreshAll() {
    lib_.refresh();
    refreshMods();
    startModsFetch();
    artCache_.clear();
    images_.clear();
    imageTried_.clear();
    std::string keep = navItems_.empty() ? std::string() : navItems_[navIndex_].key;
    user_.category = keep;
    buildNav();
    rebuildGrid();
    select(selected_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

}  // namespace fl::ui
