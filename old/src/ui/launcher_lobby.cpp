// The lobby window for games whose mod talks to the launcher's bridge
// (online.lobby in launcher.json): host or join a room, ready up, chat, and the
// host starts; every launcher then opens the game with the room's FlashVars.
// Also the first-run player name prompt and "Copy diagnostics".
#include "launcher.h"

#include <algorithm>
#include <cwctype>

#include "core/fs.h"
#include "core/log.h"
#include "core/net.h"
#include "core/strings.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>

namespace fl::ui {

using namespace detail;

namespace {

constexpr wchar_t kLobbyClass[] = L"MultiplayerEverythingLauncherLobby";
constexpr wchar_t kNameClass[] = L"MultiplayerEverythingLauncherName";

enum : int {
    kLbDirect = 600,
    kLbRoomName,
    kLbMax,
    kLbPublic,
    kLbHostPw,
    kLbPort,
    kLbUpnp,
    kLbHost,
    kLbCode,
    kLbJoinPw,
    kLbJoin,
    kLbList,
    kLbRefresh,
    kLbCopy,
    kLbPlayers,
    kLbChat,
    kLbChatEdit,
    kLbSend,
    kLbReady,
    kLbStart,
    kLbKick,
    kLbBan,
    kLbLeave,
    kLbDiag,
    kLbSetupDiag,
};

enum TextRole : LONG_PTR { kRoleText = 1, kRoleMuted, kRoleHeading, kRoleTitle, kRoleError };

constexpr int kLobbyW = 880, kLobbyH = 640;  // client size, 96-DPI units

std::wstring upperCode(const std::wstring& text) {
    std::wstring code;
    for (wchar_t ch : text)
        if (iswalnum(ch)) code += static_cast<wchar_t>(towupper(ch));
    return code;
}

}  // namespace

bool Launcher::usesLobby(int gi) const {
    return gi >= 0 && gi < static_cast<int>(cfg_.games.size()) && cfg_.games[gi].lobby.enabled && !cfg_.games[gi].mod;
}

// "<online.lobby.version>#<first 16 hex of the game file's SHA-256>": players
// with another build of the file can't join (the server compares it exactly).
std::string Launcher::lobbyVersion(int gi) {
    const GameSpec& g = cfg_.games[gi];
    ResolvedAction r = lib_.resolve(g, g.actions[joinActionIndex(g)], user_);
    std::string hash;
    if (!r.swf.empty() && !r.swfIsUrl) {
        auto it = swfHashes_.find(r.swf);
        if (it == swfHashes_.end()) it = swfHashes_.emplace(r.swf, fs::sha256File(r.swf)).first;
        hash = it->second;
    }
    return lobby::versionTag(narrow(g.lobby.version), hash);
}

// ---- the window ------------------------------------------------------------

void Launcher::openLobby(int gi) {
    if (snapshot_ && lobbyWnd_) return;
    if (!usesLobby(gi)) return;
    if (lobbyWnd_) {
        if (lobbyGame_ != gi && !(lobby_ && lobby_->running())) {
            lobbyGame_ = gi;
            SetWindowTextW(lobbyWnd_, cfg_.t("lobby.title", {{L"GAME", cfg_.games[gi].name}}).c_str());
            showLobbyPage(false);
        }
        ShowWindow(lobbyWnd_, SW_SHOWNORMAL);
        SetForegroundWindow(lobbyWnd_);
        return;
    }
    lobbyGame_ = gi;
    const GameSpec& g = cfg_.games[gi];
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = lobbyProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = theme_.iconBig;
        wc.hIconSm = theme_.iconSmall;
        wc.lpszClassName = kLobbyClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    std::wstring title = cfg_.t("lobby.title", {{L"GAME", g.name}});
    // Owned, not modal: the library stays usable while you wait in a room.
    lobbyWnd_ = CreateWindowExW(0, kLobbyClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX |
                                                                    WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, hwnd_, nullptr, inst, this);
    if (!lobbyWnd_) return;
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(lobbyWnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    HWND w = lobbyWnd_;
    lbLabels_.clear();
    lbSetup_.clear();
    lbRoom_.clear();
    auto label = [&](const std::wstring& text, TextRole role, std::vector<HWND>* page, DWORD extra = 0) {
        HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | SS_NOPREFIX | extra, 0, 0, 10, 10, w, nullptr, inst,
                                 nullptr);
        SetWindowLongPtrW(h, GWLP_USERDATA, role);
        HFONT f = role == kRoleTitle ? theme_.title : role == kRoleHeading ? theme_.heading
                                                    : role == kRoleMuted   ? theme_.caption
                                                                           : theme_.body;
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        lbLabels_.push_back(h);
        if (page) page->push_back(h);
        return h;
    };
    auto button = [&](int id, BtnStyle style, const std::wstring& text, std::vector<HWND>& page) {
        HWND b = makeButton(id, style, w);
        SetWindowTextW(b, text.c_str());
        page.push_back(b);
        return b;
    };
    auto edit = [&](int id, const std::wstring& text, std::vector<HWND>& page, DWORD extra = 0) {
        HWND e = CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL | extra, 0, 0, 10, 10, w,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), FALSE);
        page.push_back(e);
        return e;
    };
    auto list = [&](int id, std::vector<HWND>& page, DWORD extra, const std::vector<std::pair<std::wstring, int>>& cols) {
        HWND lv = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | extra,
                                  0, 0, 10, 10, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessageW(lv, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), FALSE);
        if (theme_.dark && !theme_.highContrast) {
            SetWindowTheme(lv, L"DarkMode_Explorer", nullptr);
            SetWindowTheme(ListView_GetHeader(lv), L"DarkMode_ItemsView", nullptr);
        }
        ListView_SetBkColor(lv, theme_.c.background.ref());
        ListView_SetTextBkColor(lv, theme_.c.background.ref());
        ListView_SetTextColor(lv, theme_.c.text.ref());
        for (size_t i = 0; i < cols.size(); ++i) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = const_cast<wchar_t*>(cols[i].first.c_str());
            c.cx = theme_.s(cols[i].second);
            ListView_InsertColumn(lv, static_cast<int>(i), &c);
        }
        SetWindowSubclass(lv, lobbyListProc, 0, reinterpret_cast<DWORD_PTR>(this));
        page.push_back(lv);
        return lv;
    };

    // Shared by both pages.
    lbTitle_ = label(L"", kRoleTitle, nullptr);
    lbSub_ = label(L"", kRoleMuted, nullptr, SS_LEFT);
    lbStatus_ = label(L"", kRoleMuted, nullptr, SS_LEFT);

    // Setup page; creation order is the Tab order.
    lbDirect_ = button(kLbDirect, BtnStyle::Check, cfg_.t("lobby.direct"), lbSetup_);
    label(cfg_.t("lobby.hostHead"), kRoleHeading, &lbSetup_);
    label(cfg_.t("lobby.roomName"), kRoleText, &lbSetup_);
    lbRoomName_ = edit(kLbRoomName, user_.playerName.empty() ? L"" : user_.playerName + L"'s room", lbSetup_);
    SendMessageW(lbRoomName_, EM_LIMITTEXT, 24, 0);
    label(cfg_.t("lobby.players"), kRoleText, &lbSetup_);
    lbMax_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 10, 10, w,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLbMax)), inst, nullptr);
    SendMessageW(lbMax_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), FALSE);
    if (theme_.dark && !theme_.highContrast) SetWindowTheme(lbMax_, L"DarkMode_CFD", nullptr);
    int most = std::max(2, g.maxPlayers);
    for (int n = 2; n <= most; ++n) SendMessageW(lbMax_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(std::to_wstring(n).c_str()));
    SendMessageW(lbMax_, CB_SETCURSEL, most - 2, 0);
    lbSetup_.push_back(lbMax_);
    lbPublic_ = button(kLbPublic, BtnStyle::Check, cfg_.t("lobby.public"), lbSetup_);
    SendMessageW(lbPublic_, BM_SETCHECK, BST_CHECKED, 0);
    label(cfg_.t("lobby.password"), kRoleText, &lbSetup_);
    lbHostPw_ = edit(kLbHostPw, L"", lbSetup_, ES_PASSWORD);
    label(cfg_.t("lobby.port"), kRoleText, &lbSetup_);
    lbPort_ = edit(kLbPort, std::to_wstring(g.lobby.port), lbSetup_, ES_NUMBER);
    lbUpnp_ = button(kLbUpnp, BtnStyle::Check, cfg_.t("lobby.upnp"), lbSetup_);
    button(kLbHost, BtnStyle::Primary, cfg_.t("lobby.host"), lbSetup_);
    label(cfg_.t("lobby.joinHead"), kRoleHeading, &lbSetup_);
    lbCodeLabel_ = label(cfg_.t("lobby.code"), kRoleText, &lbSetup_);
    lbCode_ = edit(kLbCode, L"", lbSetup_, ES_UPPERCASE);
    label(cfg_.t("lobby.joinPassword"), kRoleText, &lbSetup_);
    lbJoinPw_ = edit(kLbJoinPw, L"", lbSetup_, ES_PASSWORD);
    button(kLbJoin, BtnStyle::Secondary, cfg_.t("lobby.join"), lbSetup_);
    label(cfg_.t("lobby.listHead"), kRoleHeading, &lbSetup_);
    button(kLbRefresh, BtnStyle::Ghost, cfg_.t("lobby.refresh"), lbSetup_);
    lbList_ = list(kLbList, lbSetup_, 0,
                   {{cfg_.t("lobby.colCode"), 80},
                    {cfg_.t("lobby.colRoom"), 260},
                    {cfg_.t("lobby.colPlayers"), 80},
                    {cfg_.t("lobby.colPing"), 80},
                    {cfg_.t("lobby.colRegion"), 70},
                    {cfg_.t("lobby.colState"), 130}});
    button(kLbSetupDiag, BtnStyle::Ghost, cfg_.t("lobby.diag"), lbSetup_);

    // Room page.
    button(kLbCopy, BtnStyle::Secondary, cfg_.t("lobby.copyCode"), lbRoom_);
    lbPlayers_ = list(kLbPlayers, lbRoom_, 0,
                      {{cfg_.t("lobby.colSlot"), 56},
                       {cfg_.t("lobby.colName"), 200},
                       {cfg_.t("lobby.colReady"), 100},
                       {cfg_.t("lobby.colPing"), 70},
                       {cfg_.t("lobby.colStatus"), 110}});
    label(cfg_.t("lobby.chatHead"), kRoleHeading, &lbRoom_);
    lbChat_ = list(kLbChat, lbRoom_, LVS_NOCOLUMNHEADER, {{L"", 500}});
    lbChatEdit_ = edit(kLbChatEdit, L"", lbRoom_);
    SendMessageW(lbChatEdit_, EM_LIMITTEXT, lobby::kMaxChat, 0);
    button(kLbSend, BtnStyle::Secondary, cfg_.t("lobby.send"), lbRoom_);
    lbInfo_ = label(L"", kRoleText, &lbRoom_, SS_LEFT);
    button(kLbReady, BtnStyle::Primary, cfg_.t("lobby.ready"), lbRoom_);
    button(kLbStart, BtnStyle::Primary, cfg_.t("lobby.start"), lbRoom_);
    button(kLbKick, BtnStyle::Secondary, cfg_.t("lobby.kick"), lbRoom_);
    button(kLbBan, BtnStyle::Secondary, cfg_.t("lobby.ban"), lbRoom_);
    button(kLbLeave, BtnStyle::Secondary, cfg_.t("lobby.leave"), lbRoom_);
    button(kLbDiag, BtnStyle::Ghost, cfg_.t("lobby.diag"), lbRoom_);

    RECT rc{0, 0, theme_.s(kLobbyW), theme_.s(kLobbyH)};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(w, GWL_STYLE), FALSE, GetWindowLongW(w, GWL_EXSTYLE), theme_.dpi);
    RECT owner;
    GetWindowRect(hwnd_, &owner);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(w, nullptr, owner.left + (owner.right - owner.left - ww) / 2, owner.top + (owner.bottom - owner.top - wh) / 3,
                 ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
    showLobbyPage(lobby_ && lobby_->running());
    ShowWindow(w, SW_SHOW);
}

void Launcher::showLobbyPage(bool room) {
    if (!lobbyWnd_) return;
    lobbyRoomPage_ = room;
    for (HWND h : lbSetup_) ShowWindow(h, room ? SW_HIDE : SW_SHOW);
    for (HWND h : lbRoom_) ShowWindow(h, room ? SW_SHOW : SW_HIDE);
    ShowWindow(lbTitle_, SW_SHOW);
    ShowWindow(lbSub_, SW_SHOW);
    ShowWindow(lbStatus_, SW_SHOW);
    if (room) {
        lbChatShown_ = 0;
        ListView_DeleteAllItems(lbChat_);
        updateLobbyRoom();
    } else {
        const GameSpec& g = cfg_.games[lobbyGame_];
        SetWindowTextW(lbTitle_, cfg_.t("lobby.title", {{L"GAME", g.name}}).c_str());
        SetWindowTextW(lbSub_, cfg_.t("lobby.subtitle").c_str());
        SetWindowTextW(lbStatus_, lobbyMessage_.c_str());
        SetWindowLongPtrW(lbStatus_, GWLP_USERDATA, lobbyMessage_.empty() ? kRoleMuted : kRoleError);
        fillLobbyRooms();
    }
    layoutLobby();
    SetFocus(room ? GetDlgItem(lobbyWnd_, lobbySnap_.host ? kLbStart : kLbReady) : GetDlgItem(lobbyWnd_, kLbHost));
}

void Launcher::layoutLobby() {
    if (!lobbyWnd_) return;
    const Theme& t = theme_;
    int pad = t.s(24), inner = t.s(kLobbyW) - pad * 2, bottom = t.s(kLobbyH);
    auto place = [](HWND h, int x, int y, int w, int hh) {
        if (h) MoveWindow(h, x, y, w, hh, TRUE);
    };
    HDC dc = GetDC(lobbyWnd_);
    int fieldH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE) + t.s(10);
    int bh = t.s(36);
    place(lbTitle_, pad, t.s(18), inner - t.s(160), t.s(34));
    place(lbSub_, pad, t.s(54), inner, t.s(40));
    place(lbStatus_, pad, bottom - t.s(34), inner - t.s(190), t.s(24));
    bool direct = SendMessageW(lbDirect_, BM_GETCHECK, 0, 0) == BST_CHECKED;

    if (!lobbyRoomPage_) {
        int y = t.s(100);
        place(lbDirect_, pad, y, inner, t.s(30));
        y += t.s(44);
        int gap = t.s(40), colW = (inner - gap) / 2, lw = t.s(150);
        int lx = pad, rx = pad + colW + gap;
        // Labels in creation order: hostHead, roomName, players, password, port, joinHead, code, joinPassword, listHead.
        size_t li = 0;
        std::vector<HWND> labels;
        for (HWND h : lbSetup_)
            if (GetWindowLongPtrW(h, GWLP_USERDATA) >= kRoleText && !btnInfo_.count(h) && GetDlgCtrlID(h) == 0) labels.push_back(h);
        auto next = [&] { return li < labels.size() ? labels[li++] : nullptr; };
        int ly = y;
        place(next(), lx, ly, colW, t.s(28));  // Host a room
        ly += t.s(36);
        place(next(), lx, ly + t.s(6), lw, t.s(22));
        place(lbRoomName_, lx + lw, ly, colW - lw, fieldH);
        ly += fieldH + t.s(10);
        place(next(), lx, ly + t.s(6), lw, t.s(22));
        place(lbMax_, lx + lw, ly, t.s(90), t.s(240));
        ly += fieldH + t.s(10);
        place(lbPublic_, lx, ly, colW, t.s(30));
        ShowWindow(lbPublic_, direct ? SW_HIDE : SW_SHOW);
        if (!direct) ly += t.s(38);
        place(next(), lx, ly + t.s(6), lw, t.s(22));
        place(lbHostPw_, lx + lw, ly, colW - lw, fieldH);
        ly += fieldH + t.s(10);
        HWND portLabel = next();
        place(portLabel, lx, ly + t.s(6), lw, t.s(22));
        place(lbPort_, lx + lw, ly, t.s(90), fieldH);
        ShowWindow(portLabel, direct ? SW_SHOW : SW_HIDE);
        ShowWindow(lbPort_, direct ? SW_SHOW : SW_HIDE);
        if (direct) ly += fieldH + t.s(10);
        place(lbUpnp_, lx, ly, colW, t.s(30));
        ShowWindow(lbUpnp_, direct ? SW_SHOW : SW_HIDE);
        if (direct) ly += t.s(38);
        place(GetDlgItem(lobbyWnd_, kLbHost), lx, ly + t.s(4), t.s(150), bh);
        int hostBottom = ly + t.s(4) + bh;

        int ry = y;
        place(next(), rx, ry, colW, t.s(28));  // Join a room
        ry += t.s(36);
        SetWindowTextW(lbCodeLabel_, cfg_.t(direct ? "lobby.address" : "lobby.code").c_str());
        place(next(), rx, ry + t.s(6), lw, t.s(22));
        place(lbCode_, rx + lw, ry, colW - lw, fieldH);
        LONG_PTR style = GetWindowLongPtrW(lbCode_, GWL_STYLE);
        SetWindowLongPtrW(lbCode_, GWL_STYLE, direct ? style & ~ES_UPPERCASE : style | ES_UPPERCASE);
        ry += fieldH + t.s(10);
        place(next(), rx, ry + t.s(6), lw, t.s(22));
        place(lbJoinPw_, rx + lw, ry, colW - lw, fieldH);
        ry += fieldH + t.s(10);
        place(GetDlgItem(lobbyWnd_, kLbJoin), rx, ry + t.s(4), t.s(150), bh);

        int listY = std::max(hostBottom, ry + t.s(4) + bh) + t.s(26);
        HWND listHead = next();
        place(listHead, pad, listY, t.s(300), t.s(28));
        place(GetDlgItem(lobbyWnd_, kLbRefresh), pad + inner - t.s(120), listY - t.s(4), t.s(120), t.s(32));
        place(lbList_, pad, listY + t.s(36), inner, bottom - t.s(50) - (listY + t.s(36)));
        RECT lc;
        GetClientRect(lbList_, &lc);
        int fixed = 0;
        for (int c : {0, 2, 3, 4, 5}) fixed += ListView_GetColumnWidth(lbList_, c);
        ListView_SetColumnWidth(lbList_, 1, std::max(t.s(120), static_cast<int>(lc.right) - fixed - t.s(2)));
        for (HWND h : {listHead, GetDlgItem(lobbyWnd_, kLbRefresh), lbList_}) ShowWindow(h, direct ? SW_HIDE : SW_SHOW);
        place(GetDlgItem(lobbyWnd_, kLbSetupDiag), pad + inner - t.s(180), bottom - t.s(40), t.s(180), t.s(32));
    } else {
        int y = t.s(104);
        int rightW = t.s(290), gap = t.s(28), leftW = inner - rightW - gap, rx = pad + leftW + gap;
        place(GetDlgItem(lobbyWnd_, kLbCopy), pad + inner - t.s(140), t.s(20), t.s(140), t.s(34));
        ShowWindow(GetDlgItem(lobbyWnd_, kLbCopy), lobbySnap_.transport == lobby::Transport::Online ? SW_SHOW : SW_HIDE);
        int playersH = t.s(176);
        place(lbPlayers_, pad, y, leftW, playersH);
        // The name column takes what the fixed ones leave, so there's never a sideways scroll bar.
        RECT lc;
        GetClientRect(lbPlayers_, &lc);
        int fixed = 0;
        for (int c : {0, 2, 3, 4}) fixed += ListView_GetColumnWidth(lbPlayers_, c);
        ListView_SetColumnWidth(lbPlayers_, 1, std::max(t.s(80), static_cast<int>(lc.right) - fixed - t.s(2)));
        int cy = y + playersH + t.s(18);
        HWND chatHead = nullptr;
        for (HWND h : lbRoom_)
            if (GetWindowLongPtrW(h, GWLP_USERDATA) == kRoleHeading) chatHead = h;
        place(chatHead, pad, cy, leftW, t.s(28));
        cy += t.s(34);
        int inputY = bottom - t.s(50) - fieldH;
        place(lbChat_, pad, cy, leftW, inputY - t.s(10) - cy);
        RECT cc;
        GetClientRect(lbChat_, &cc);
        ListView_SetColumnWidth(lbChat_, 0, cc.right - GetSystemMetrics(SM_CXVSCROLL));
        place(lbChatEdit_, pad, inputY, leftW - t.s(100), fieldH);
        place(GetDlgItem(lobbyWnd_, kLbSend), pad + leftW - t.s(90), inputY - t.s(2), t.s(90), fieldH + t.s(4));

        std::wstring info = windowText(lbInfo_);
        int infoH = std::min(t.s(250), textHeight(dc, t.body, info, rightW) + t.s(4));
        place(lbInfo_, rx, y, rightW, infoH);
        int by = y + infoH + t.s(16);
        bool host = lobbySnap_.host;
        HWND ready = GetDlgItem(lobbyWnd_, kLbReady), start = GetDlgItem(lobbyWnd_, kLbStart);
        ShowWindow(ready, host ? SW_HIDE : SW_SHOW);
        ShowWindow(start, host ? SW_SHOW : SW_HIDE);
        place(host ? start : ready, rx, by, rightW, t.s(44));
        by += t.s(54);
        HWND kick = GetDlgItem(lobbyWnd_, kLbKick), ban = GetDlgItem(lobbyWnd_, kLbBan);
        ShowWindow(kick, host ? SW_SHOW : SW_HIDE);
        ShowWindow(ban, host ? SW_SHOW : SW_HIDE);
        if (host) {
            place(kick, rx, by, (rightW - t.s(8)) / 2, bh);
            place(ban, rx + (rightW + t.s(8)) / 2, by, (rightW - t.s(8)) / 2, bh);
            by += bh + t.s(10);
        }
        place(GetDlgItem(lobbyWnd_, kLbLeave), rx, by, rightW, bh);
        place(GetDlgItem(lobbyWnd_, kLbDiag), pad + inner - t.s(180), bottom - t.s(40), t.s(180), t.s(32));
    }
    ReleaseDC(lobbyWnd_, dc);
    InvalidateRect(lobbyWnd_, nullptr, TRUE);
}

void Launcher::fillLobbyRooms() {
    if (!lobbyWnd_ || lobbyRoomPage_ || lobbyGame_ < 0) return;
    const GameSpec& g = cfg_.games[lobbyGame_];
    std::wstring selected;
    int sel = ListView_GetNextItem(lbList_, -1, LVNI_SELECTED);
    if (sel >= 0) {
        wchar_t buf[16] = {};
        ListView_GetItemText(lbList_, sel, 0, buf, 16);
        selected = buf;
    }
    SendMessageW(lbList_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lbList_);
    int row = 0;
    if (auto it = rooms_.find(g.id); it != rooms_.end())
        for (const auto& r : it->second) {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(r.code.c_str());
            item.lParam = r.inGame() ? 1 : 0;
            ListView_InsertItem(lbList_, &item);
            ListView_SetItemText(lbList_, row, 1, const_cast<wchar_t*>(r.name.c_str()));
            std::wstring players = std::to_wstring(r.players) + L"/" + std::to_wstring(r.max);
            ListView_SetItemText(lbList_, row, 2, players.data());
            // Your way to the server plus the host's: roughly what the two of you will feel.
            std::wstring ping = pingMs_ >= 0 && r.hostPing >= 0 ? std::to_wstring(pingMs_ + r.hostPing) + L" ms"
                                : pingMs_ >= 0                ? std::to_wstring(pingMs_) + L"+ ms"
                                                              : L"-";
            ListView_SetItemText(lbList_, row, 3, ping.data());
            std::wstring region = r.region.empty() ? L"-" : r.region;
            ListView_SetItemText(lbList_, row, 4, region.data());
            std::wstring state = r.inGame() ? cfg_.t("lobby.statePlaying") : cfg_.t("lobby.stateLobby");
            if (r.locked) state += L", " + toLower(cfg_.t("lobby.locked"));
            ListView_SetItemText(lbList_, row, 5, state.data());
            if (r.code == selected) ListView_SetItemState(lbList_, row, LVIS_SELECTED, LVIS_SELECTED);
            ++row;
        }
    SendMessageW(lbList_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lbList_, nullptr, TRUE);
}

// ---- hosting and joining -----------------------------------------------------

namespace {

lobby::Options baseOptions(const Config& cfg, const GameSpec& g, const UserData& user, const std::string& version) {
    lobby::Options o;
    o.gameId = g.onlineId;
    o.version = version;
    o.playerName = user.playerName;
    o.maxPlayers = std::max(2, g.maxPlayers);
    o.region = net::userRegion();
    (void)cfg;
    return o;
}

}  // namespace

void Launcher::lobbyHost() {
    if (lobbyGame_ < 0 || (user_.playerName.empty() && !askName(false))) return;
    const GameSpec& g = cfg_.games[lobbyGame_];
    if (lib_.resolve(g, g.actions[joinActionIndex(g)], user_).state == ActionState::NoSwf) {
        lobbyMessage_ = cfg_.t("lobby.noGameFile");
        showLobbyPage(false);
        offerGameDownload(g);
        return;
    }
    bool direct = SendMessageW(lbDirect_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    lobby::Options o = baseOptions(cfg_, g, user_, lobbyVersion(lobbyGame_));
    o.host = true;
    o.transport = direct ? lobby::Transport::Direct : lobby::Transport::Online;
    o.roomName = trim(windowText(lbRoomName_));
    o.password = windowText(lbHostPw_);
    o.isPublic = SendMessageW(lbPublic_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    o.maxPlayers = static_cast<int>(SendMessageW(lbMax_, CB_GETCURSEL, 0, 0)) + 2;
    o.directPort = std::clamp(_wtoi(windowText(lbPort_).c_str()), 1, 65535);
    if (!direct) {
        std::lock_guard<std::mutex> lock(installMu_);
        o.server = roomServer_.empty() ? cfg_.rooms.server : roomServer_;
    }
    lobby_ = std::make_unique<lobby::Session>();
    HWND wnd = hwnd_;
    lobby_->setOnChange([wnd] { PostMessageW(wnd, WM_APP_LOBBY, 0, 0); });
    std::wstring err;
    if (!lobby_->start(o, err)) {
        lobby_.reset();
        lobbyMessage_ = cfg_.t("lobby.r.unreachable", {{L"DETAIL", err}});
        showLobbyPage(false);
        return;
    }
    lobbyMessage_.clear();
    lobbyNetText_.clear();
    lobbySnap_ = lobby_->snapshot();
    if (pingMs_ >= 0) lobby_->setServerPing(pingMs_);
    if (direct) startDirectChecks(o.directPort);
    showLobbyPage(true);
}

void Launcher::lobbyJoin(const std::wstring& codeOrAddress, const std::wstring& password, bool direct) {
    if (lobbyGame_ < 0 || (user_.playerName.empty() && !askName(false))) return;
    const GameSpec& g = cfg_.games[lobbyGame_];
    if (lib_.resolve(g, g.actions[joinActionIndex(g)], user_).state == ActionState::NoSwf) {
        lobbyMessage_ = cfg_.t("lobby.noGameFile");
        showLobbyPage(false);
        offerGameDownload(g);
        return;
    }
    lobby::Options o = baseOptions(cfg_, g, user_, lobbyVersion(lobbyGame_));
    o.password = password;
    if (direct) {
        std::wstring addr = trim(codeOrAddress);
        if (addr.empty()) {
            lobbyMessage_ = cfg_.t("lobby.badAddress");
            showLobbyPage(false);
            return;
        }
        if (addr.find(L':') == std::wstring::npos) addr += L":" + std::to_wstring(g.lobby.port);
        o.transport = lobby::Transport::Direct;
        o.server = addr;
    } else {
        std::wstring code = upperCode(codeOrAddress);
        if (code.size() != 5) {
            lobbyMessage_ = cfg_.t("lobby.badCode");
            showLobbyPage(false);
            return;
        }
        o.code = code;
        std::lock_guard<std::mutex> lock(installMu_);
        o.server = roomServer_.empty() ? cfg_.rooms.server : roomServer_;
    }
    lobby_ = std::make_unique<lobby::Session>();
    HWND wnd = hwnd_;
    lobby_->setOnChange([wnd] { PostMessageW(wnd, WM_APP_LOBBY, 0, 0); });
    std::wstring err;
    if (!lobby_->start(o, err)) {
        lobby_.reset();
        lobbyMessage_ = cfg_.t("lobby.r.unreachable", {{L"DETAIL", err}});
        showLobbyPage(false);
        return;
    }
    lobbyMessage_.clear();
    lobbySnap_ = lobby_->snapshot();
    showLobbyPage(true);
}

void Launcher::lobbyQuick(int gi) {
    openLobby(gi);
    if (!lobbyWnd_ || (lobby_ && lobby_->running())) return;
    const GameSpec& g = cfg_.games[gi];
    const RoomInfo* best = nullptr;
    if (auto it = rooms_.find(g.id); it != rooms_.end())
        for (const auto& r : it->second)
            if (!r.locked && !r.inGame() && (r.max <= 0 || r.players < r.max) && (!best || r.players > best->players)) best = &r;
    if (best) lobbyJoin(best->code, L"", false);
    else lobbyHost();
}

void Launcher::closeLobby() {
    if (lobby_) lobby_->stop();
    lobby_.reset();
    lobbySnap_ = {};
    if (lobbyUpnpPort_) {
        int port = lobbyUpnpPort_;
        lobbyUpnpPort_ = 0;
        if (lobbyNetThread_.joinable()) lobbyNetThread_.join();
        lobbyNetThread_ = std::thread([port] { net::removePortMapping(port); });
    }
}

// ---- the session ----------------------------------------------------------------

std::wstring Launcher::lobbyReasonText(const lobby::Snapshot& s) const {
    using lobby::Reason;
    switch (s.reason) {
        case Reason::HostLeft: return cfg_.t("lobby.r.hostLeft");
        case Reason::Kicked: return cfg_.t("lobby.r.kicked");
        case Reason::Banned: return cfg_.t("lobby.r.banned");
        case Reason::Full: return cfg_.t("lobby.r.full");
        case Reason::InGame: return cfg_.t("lobby.r.ingame");
        case Reason::Version:
            return cfg_.t("lobby.r.version", {{L"HOST", widen(lobby::versionName(narrow(s.hostVersion)))}});
        case Reason::Password: return cfg_.t("lobby.r.password");
        case Reason::TooManyTries: return cfg_.t("lobby.r.tries");
        case Reason::NoRoom: return cfg_.t("lobby.r.noRoom");
        case Reason::Unreachable: return cfg_.t("lobby.r.unreachable", {{L"DETAIL", s.detail}});
        case Reason::Lost: return cfg_.t("lobby.r.lost");
        case Reason::Server: return cfg_.t("lobby.r.server", {{L"DETAIL", s.detail}});
        case Reason::None: break;
    }
    return L"";
}

void Launcher::onLobby() {
    if (!lobby_) return;
    lobby::Snapshot s = lobby_->snapshot();
    if (s.revision == lobbySnap_.revision && s.revision != 0) return;
    lobbySnap_ = s;
    if (s.startNow && !lobbyPid_) launchFromLobby();
    if (s.phase == lobby::Phase::Closed) {
        lobbyMessage_ = lobbyReasonText(s);
        logger().info(L"Lobby closed: " + (lobbyMessage_.empty() ? std::wstring(L"left") : lobbyMessage_));
        bool version = s.reason == lobby::Reason::Version;
        closeLobby();
        if (lobbyWnd_) showLobbyPage(false);
        if (version && lobbyGame_ >= 0 && !outdatedFor(cfg_.games[lobbyGame_]).empty() &&
            MessageBoxW(lobbyWnd_ ? lobbyWnd_ : hwnd_, cfg_.t("lobby.versionUpdate").c_str(), cfg_.games[lobbyGame_].name.c_str(),
                        MB_YESNO | MB_ICONQUESTION) == IDYES) {
            swfHashes_.clear();
            offerGameDownload(cfg_.games[lobbyGame_]);
        }
        return;
    }
    if (lobbyWnd_ && lobbyRoomPage_) updateLobbyRoom();
}

void Launcher::updateLobbyRoom() {
    if (!lobbyWnd_) return;
    const lobby::Snapshot& s = lobbySnap_;
    using lobby::Phase;
    bool online = s.transport == lobby::Transport::Online;
    std::wstring title = online ? (s.code.empty() ? cfg_.t("lobby.phaseConnecting") : cfg_.t("lobby.room", {{L"CODE", s.code}}))
                                : cfg_.t("lobby.directRoom");
    if (windowText(lbTitle_) != title) SetWindowTextW(lbTitle_, title.c_str());
    std::wstring sub = s.roomName;
    if (!online && !s.host) sub = s.code;
    if (s.locked) sub += (sub.empty() ? L"" : L", ") + toLower(cfg_.t("lobby.locked"));
    SetWindowTextW(lbSub_, sub.c_str());

    // Players, keeping the selection on the same slot.
    int selSlot = -1;
    int sel = ListView_GetNextItem(lbPlayers_, -1, LVNI_SELECTED);
    if (sel >= 0) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = sel;
        ListView_GetItem(lbPlayers_, &it);
        selSlot = static_cast<int>(it.lParam);
    }
    SendMessageW(lbPlayers_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lbPlayers_);
    int row = 0;
    bool meReady = false;
    for (const auto& p : s.players) {
        std::wstring slot = L"P" + std::to_wstring(p.slot + 1);
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = slot.data();
        item.lParam = p.slot;
        ListView_InsertItem(lbPlayers_, &item);
        std::wstring name = p.you ? cfg_.t("lobby.you", {{L"NAME", p.name}}) : p.name;
        if (p.host) name += L"  [" + cfg_.t("lobby.hostTag") + L"]";
        ListView_SetItemText(lbPlayers_, row, 1, name.data());
        std::wstring ready = p.host ? L"" : p.ready ? cfg_.t("lobby.isReady") : cfg_.t("lobby.notReady");
        ListView_SetItemText(lbPlayers_, row, 2, ready.data());
        std::wstring ping = p.pingMs >= 0 ? std::to_wstring(p.pingMs) + L" ms" : L"-";
        ListView_SetItemText(lbPlayers_, row, 3, ping.data());
        std::wstring status = !p.connected                                         ? cfg_.t("lobby.away")
                              : s.phase == Phase::Starting                         ? (p.loaded ? cfg_.t("lobby.loaded") : cfg_.t("lobby.loading"))
                              : s.phase == Phase::InGame                           ? cfg_.t("lobby.statePlaying")
                                                                                   : L"";
        ListView_SetItemText(lbPlayers_, row, 4, status.data());
        if (p.slot == selSlot) ListView_SetItemState(lbPlayers_, row, LVIS_SELECTED, LVIS_SELECTED);
        if (p.you) meReady = p.ready;
        ++row;
    }
    SendMessageW(lbPlayers_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(lbPlayers_, nullptr, TRUE);

    // Chat: only what's new (the session keeps the last 100 lines).
    if (s.chat.size() < lbChatShown_) {
        ListView_DeleteAllItems(lbChat_);
        lbChatShown_ = 0;
    }
    for (size_t i = lbChatShown_; i < s.chat.size(); ++i) {
        const auto& c = s.chat[i];
        std::wstring text = c.slot < 0 ? c.text : c.name + L": " + c.text;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = text.data();
        item.lParam = c.slot;
        ListView_InsertItem(lbChat_, &item);
    }
    if (s.chat.size() != lbChatShown_) ListView_EnsureVisible(lbChat_, static_cast<int>(s.chat.size()) - 1, FALSE);
    lbChatShown_ = s.chat.size();

    // What's happening, plus how friends reach a direct host.
    std::wstring info = s.reconnecting                ? cfg_.t("lobby.reconnecting")
                        : s.phase == Phase::Connecting ? cfg_.t("lobby.phaseConnecting")
                        : s.phase == Phase::Starting   ? cfg_.t("lobby.phaseStarting")
                        : s.phase == Phase::InGame     ? cfg_.t("lobby.phaseInGame")
                        : s.host                       ? cfg_.t("lobby.phaseLobbyHost")
                                                       : cfg_.t("lobby.phaseLobby");
    if (s.host && s.phase == Phase::Lobby && !s.reconnecting) {
        std::wstring waiting;
        for (const auto& p : s.players)
            if (!p.host && p.connected && !p.ready) waiting += (waiting.empty() ? L"" : L", ") + p.name;
        if (s.players.size() < 2) info = cfg_.t("lobby.waitFriends");
        else if (!waiting.empty()) info += L" " + cfg_.t("lobby.waitingFor", {{L"NAMES", waiting}});
    }
    if (!online && s.host) {
        int port = cfg_.games[lobbyGame_].lobby.port;
        if (size_t sp = s.code.rfind(L' '); sp != std::wstring::npos) port = _wtoi(s.code.c_str() + sp + 1);
        info += L"\n\n" + cfg_.t("lobby.addrHead");
        int shown = 0;
        for (const auto& a : net::shareAddresses(sys::localAddresses())) {
            if (shown++ == 4) break;
            Vars v;
            std::wstring key = a.kind == net::AddressKind::Vpn ? L"lobby.addrVpn" : a.kind == net::AddressKind::Lan ? L"lobby.addrLan" : L"lobby.addrOther";
            info += L"\n" + cfg_.t(narrow(key), {{L"ADDR", a.address}, {L"PORT", std::to_wstring(port)}, {L"VPN", a.vpn}});
        }
        if (!lobbyNetText_.empty()) info += L"\n\n" + lobbyNetText_;
    }
    if (windowText(lbInfo_) != info) {
        SetWindowTextW(lbInfo_, info.c_str());
        layoutLobby();
    }

    HWND ready = GetDlgItem(lobbyWnd_, kLbReady), start = GetDlgItem(lobbyWnd_, kLbStart);
    std::wstring readyText = cfg_.t(meReady ? "lobby.unready" : "lobby.ready");
    if (windowText(ready) != readyText) {
        SetWindowTextW(ready, readyText.c_str());
        btnInfo_[ready].style = meReady ? BtnStyle::Secondary : BtnStyle::Primary;
        InvalidateRect(ready, nullptr, FALSE);
    }
    bool inLobby = s.phase == Phase::Lobby;
    bool allReady = true;
    for (const auto& p : s.players)
        if (!p.host && p.connected && !p.ready) allReady = false;
    EnableWindow(ready, inLobby);
    EnableWindow(start, inLobby);
    btnInfo_[start].ready = allReady && inLobby;  // drawn as not ready while a round runs, too
    InvalidateRect(start, nullptr, FALSE);
    bool canKick = s.host && selSlot > 0;
    EnableWindow(GetDlgItem(lobbyWnd_, kLbKick), canKick);
    EnableWindow(GetDlgItem(lobbyWnd_, kLbBan), canKick);
    SetWindowTextW(lbStatus_, L"");
}

void Launcher::launchFromLobby() {
    if (!lobby_ || lobbyGame_ < 0) return;
    const lobby::Snapshot& s = lobbySnap_;
    const GameSpec& g = cfg_.games[lobbyGame_];
    FlashVars fv = {{L"room", s.code},
                    {L"role", s.host ? L"host" : L"client"},
                    {L"slot", std::to_wstring(s.mySlot)},
                    {L"bridgePort", std::to_wstring(s.bridgePort)},
                    {L"sessionToken", s.token},
                    {L"seed", std::to_wstring(s.seed)},
                    {L"lobby", L"1"}};
    size_t before = sessions_.size();
    if (runAction(lobbyGame_, joinActionIndex(g), fv) && sessions_.size() > before) {
        lobbyPid_ = sessions_.back().pid;
        lobby_->gameLaunched();
        logger().info(L"Lobby: started " + g.name + L" as slot " + std::to_wstring(s.mySlot));
    } else {
        lobby_->gameExited();  // the host goes back to the lobby; a guest tells the host
        if (lobbyWnd_) {
            SetWindowTextW(lbStatus_, notice_.c_str());
            SetWindowLongPtrW(lbStatus_, GWLP_USERDATA, kRoleError);
        }
    }
}

void Launcher::startDirectChecks(int port) {
    bool upnp = lobbyWnd_ && SendMessageW(lbUpnp_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring server;
    {
        std::lock_guard<std::mutex> lock(installMu_);
        server = roomServer_.empty() ? cfg_.rooms.server : roomServer_;
    }
    std::string gameId = lobbyGame_ >= 0 ? cfg_.games[lobbyGame_].onlineId : std::string();
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exePath = exe;
    std::wstring lan;
    for (const auto& a : net::shareAddresses(sys::localAddresses()))
        if (a.kind == net::AddressKind::Lan) {
            lan = a.address;
            break;
        }
    if (upnp) lobbyUpnpPort_ = port;
    lobbyNetText_ = cfg_.t("lobby.portTesting", {{L"PORT", std::to_wstring(port)}});
    if (lobbyNetThread_.joinable()) lobbyNetThread_.join();
    HWND wnd = hwnd_;
    const Config* cfg = &cfg_;
    lobbyNetThread_ = std::thread([=] {
        std::vector<std::wstring> lines;
        std::wstring P = std::to_wstring(port);
        if (upnp) {
            std::wstring err;
            if (!lan.empty() && net::addPortMapping(port, lan, L"Multiplayer Everything", err))
                lines.push_back(cfg->t("lobby.upnpOk", {{L"PORT", P}}));
            else
                lines.push_back(cfg->t("lobby.upnpFailed", {{L"ERROR", err.empty() ? L"no home network address" : err}}));
        }
        if (net::firewallAllows(exePath, port) == net::Firewall::Blocked)
            lines.push_back(cfg->t("lobby.firewallBlocked", {{L"PORT", P}}));
        std::wstring err;
        std::string result = server.empty() ? "" : probePort(server, gameId, port, err);
        lines.push_back(result == "open"     ? cfg->t("lobby.portOpen", {{L"PORT", P}})
                        : result == "closed" ? cfg->t("lobby.portClosed", {{L"PORT", P}})
                                             : cfg->t("lobby.portUnknown", {{L"PORT", P}}));
        std::wstring text;
        for (const auto& l : lines) text += (text.empty() ? L"" : L"\n") + l;
        PostMessageW(wnd, WM_APP_LOBBYNET, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
    });
}

// The lists' column headers and the empty-list line: the dark list theme draws
// both in a gray too dim to read, so they get the theme's own text colors.
LRESULT CALLBACK Launcher::lobbyListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Launcher*>(ref);
    const Theme& t = self->theme_;
    if (m == WM_NOTIFY) {
        auto* hdr = reinterpret_cast<NMHDR*>(l);
        if (hdr->hwndFrom == ListView_GetHeader(h) && hdr->code == NM_CUSTOMDRAW) {
            auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, t.c.textMuted.ref());
                return CDRF_DODEFAULT;
            }
            return CDRF_DODEFAULT;
        }
    }
    if (m == WM_PAINT && h == self->lbList_ && ListView_GetItemCount(h) == 0) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        RECT rc, head{};
        GetClientRect(h, &rc);
        if (HWND hd = ListView_GetHeader(h)) GetWindowRect(hd, &head);
        rc.top += head.bottom - head.top;
        HDC dc = GetDC(h);
        HGDIOBJ old = SelectObject(dc, t.body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, t.c.textMuted.ref());  // 6.9:1 on the list background
        std::wstring text = self->cfg_.t("lobby.listEmpty");
        DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        ReleaseDC(h, dc);
        return r;
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, lobbyListProc, 0);
    return DefSubclassProc(h, m, w, l);
}

// ---- messages ---------------------------------------------------------------------

LRESULT CALLBACK Launcher::lobbyProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->lobbyHandle(h, m, w, l);
}

LRESULT Launcher::lobbyHandle(HWND h, UINT m, WPARAM w, LPARAM l) {
    const Theme& t = theme_;
    switch (m) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, t.brSurface);
            return 1;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            // The player colors along the top: the one bold element of the design.
            RECT rc;
            GetClientRect(h, &rc);
            const auto& colors = cfg_.branding.playerColors;
            int n = static_cast<int>(colors.size());
            for (int i = 0; i < n; ++i) {
                RECT bar{rc.right * i / n, 0, rc.right * (i + 1) / n, t.s(4)};
                HBRUSH br = CreateSolidBrush(colors[i].ref());
                FillRect(dc, &bar, br);
                DeleteObject(br);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            LONG_PTR role = GetWindowLongPtrW(reinterpret_cast<HWND>(l), GWLP_USERDATA);
            SetTextColor(dc, (role == kRoleMuted ? t.c.textMuted : role == kRoleError ? t.c.error : t.c.text).ref());
            SetBkColor(dc, t.c.surface.ref());
            return reinterpret_cast<LRESULT>(t.brSurface);
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), t.c.text.ref());
            SetBkColor(reinterpret_cast<HDC>(w), t.c.background.ref());
            return reinterpret_cast<LRESULT>(t.brBackground);
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(w), t.c.text.ref());
            SetBkColor(reinterpret_cast<HDC>(w), t.c.surface.ref());
            return reinterpret_cast<LRESULT>(t.brSurface);
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->code == NM_CUSTOMDRAW && btnInfo_.count(hdr->hwndFrom)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    drawButton(hdr->hwndFrom, cd->hdc, cd->rc, cd->uItemState);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == lbPlayers_ || hdr->hwndFrom == lbChat_ || hdr->hwndFrom == lbList_)) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    if (hdr->hwndFrom == lbPlayers_) return CDRF_NOTIFYSUBITEMDRAW;
                    int key = static_cast<int>(cd->nmcd.lItemlParam);
                    if (hdr->hwndFrom == lbChat_)
                        cd->clrText = key < 0 ? t.c.textMuted.ref() : t.c.text.ref();
                    else
                        cd->clrText = key == 1 ? t.c.textMuted.ref() : t.c.text.ref();  // rooms already playing are greyed
                    return CDRF_NEWFONT;
                }
                if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                    int slot = static_cast<int>(cd->nmcd.lItemlParam);
                    const auto& colors = cfg_.branding.playerColors;
                    // The slot column in the player's own color: 4.5:1 or better on the dark list background,
                    // not on the light one, which keeps plain text.
                    cd->clrText = cd->iSubItem == 0 && !colors.empty() && t.dark && !t.highContrast
                                      ? colors[static_cast<size_t>(slot) % colors.size()].ref()
                                      : t.c.text.ref();
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->hwndFrom == lbList_ && (hdr->code == NM_DBLCLK || hdr->code == LVN_KEYDOWN)) {
                if (hdr->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(l)->wVKey != VK_RETURN) break;
                int sel = ListView_GetNextItem(lbList_, -1, LVNI_SELECTED);
                if (sel >= 0) {
                    wchar_t code[16] = {};
                    ListView_GetItemText(lbList_, sel, 0, code, 16);
                    lobbyJoin(code, windowText(lbJoinPw_), false);
                }
                return 0;
            }
            if (hdr->hwndFrom == lbPlayers_ && hdr->code == LVN_ITEMCHANGED) {
                updateLobbyRoom();
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (code != BN_CLICKED) break;
            switch (id) {
                case kLbDirect:
                case kLbUpnp:
                case kLbPublic:
                    layoutLobby();
                    return 0;
                case kLbHost:
                    lobbyHost();
                    return 0;
                case kLbJoin:
                    lobbyJoin(windowText(lbCode_), windowText(lbJoinPw_), SendMessageW(lbDirect_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    return 0;
                case kLbRefresh:
                    if (roomsWake_) SetEvent(roomsWake_);
                    return 0;
                case kLbCopy:
                    if (!lobbySnap_.code.empty()) {
                        copyText(lobbySnap_.code);
                        SetWindowTextW(lbStatus_, cfg_.t("lobby.copied", {{L"CODE", lobbySnap_.code}}).c_str());
                        SetWindowLongPtrW(lbStatus_, GWLP_USERDATA, kRoleMuted);
                    }
                    return 0;
                case kLbSend: {
                    std::wstring text = trim(windowText(lbChatEdit_));
                    if (!text.empty() && lobby_) lobby_->sendChat(text);
                    SetWindowTextW(lbChatEdit_, L"");
                    SetFocus(lbChatEdit_);
                    return 0;
                }
                case kLbReady: {
                    bool me = false;
                    for (const auto& p : lobbySnap_.players)
                        if (p.you) me = p.ready;
                    if (lobby_) lobby_->setReady(!me);
                    return 0;
                }
                case kLbStart:
                    if (!btnInfo_[GetDlgItem(h, kLbStart)].ready) {
                        SetWindowTextW(lbStatus_, cfg_.t("lobby.everyoneReady").c_str());
                        SetWindowLongPtrW(lbStatus_, GWLP_USERDATA, kRoleError);
                        return 0;
                    }
                    if (lobby_) lobby_->startGame();
                    return 0;
                case kLbKick:
                case kLbBan: {
                    int sel = ListView_GetNextItem(lbPlayers_, -1, LVNI_SELECTED);
                    if (sel < 0 || !lobby_) {
                        SetWindowTextW(lbStatus_, cfg_.t("lobby.pickPlayer").c_str());
                        return 0;
                    }
                    LVITEMW it{};
                    it.mask = LVIF_PARAM;
                    it.iItem = sel;
                    ListView_GetItem(lbPlayers_, &it);
                    if (it.lParam > 0) lobby_->kick(static_cast<int>(it.lParam), id == kLbBan);
                    return 0;
                }
                case kLbLeave:
                    lobbyMessage_.clear();
                    closeLobby();
                    showLobbyPage(false);
                    return 0;
                case kLbDiag:
                case kLbSetupDiag:
                    copyDiagnostics();
                    SetWindowTextW(lbStatus_, cfg_.t("lobby.diagCopied").c_str());
                    SetWindowLongPtrW(lbStatus_, GWLP_USERDATA, kRoleMuted);
                    InvalidateRect(lbStatus_, nullptr, TRUE);
                    return 0;
                case IDCANCEL:  // Esc
                    if (lobbyRoomPage_) return 0;  // leaving a room takes the button, not a stray key
                    DestroyWindow(h);
                    return 0;
                case IDOK:  // Enter: the field with focus says what it means
                {
                    HWND f = GetFocus();
                    if (f == lbChatEdit_) SendMessageW(h, WM_COMMAND, MAKEWPARAM(kLbSend, BN_CLICKED), 0);
                    else if (f == lbCode_ || f == lbJoinPw_) SendMessageW(h, WM_COMMAND, MAKEWPARAM(kLbJoin, BN_CLICKED), 0);
                    else if (f == lbRoomName_ || f == lbHostPw_ || f == lbPort_) SendMessageW(h, WM_COMMAND, MAKEWPARAM(kLbHost, BN_CLICKED), 0);
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            closeLobby();
            for (auto it = btnInfo_.begin(); it != btnInfo_.end();)
                it = GetParent(it->first) == h ? btnInfo_.erase(it) : std::next(it);
            if (hoverBtn_ && GetParent(hoverBtn_) == h) hoverBtn_ = nullptr;
            lobbyWnd_ = nullptr;
            lbSetup_.clear();
            lbRoom_.clear();
            lbLabels_.clear();
            lobbyMessage_.clear();
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- player name ----------------------------------------------------------------

namespace {
struct NameDialog {
    Launcher* self = nullptr;
    HWND edit = nullptr, ok = nullptr;
    std::wstring result;
    bool done = false;
    bool address = false;  // the field takes "host:port" instead of a player name
    bool code = false;     // ...or a room code too (the Join box)
    RECT field{};          // the box around the edit, painted by the dialog
};
}  // namespace

LRESULT CALLBACK Launcher::nameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<NameDialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        d = static_cast<NameDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    }
    if (!d) return DefWindowProcW(h, m, w, l);
    Launcher& L = *d->self;
    const Theme& t = L.theme_;
    switch (m) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, t.brSurface);
            if (d->field.right > d->field.left) FillRect(reinterpret_cast<HDC>(w), &d->field, t.brBackground);
            const auto& colors = L.cfg_.branding.playerColors;
            int n = static_cast<int>(colors.size());
            for (int i = 0; i < n; ++i) {
                RECT bar{rc.right * i / n, 0, rc.right * (i + 1) / n, t.s(4)};
                HBRUSH br = CreateSolidBrush(colors[i].ref());
                FillRect(reinterpret_cast<HDC>(w), &bar, br);
                DeleteObject(br);
            }
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            LONG_PTR role = GetWindowLongPtrW(reinterpret_cast<HWND>(l), GWLP_USERDATA);
            SetTextColor(dc, (role == kRoleMuted ? t.c.textMuted : t.c.text).ref());
            SetBkColor(dc, t.c.surface.ref());
            return reinterpret_cast<LRESULT>(t.brSurface);
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), t.c.text.ref());
            SetBkColor(reinterpret_cast<HDC>(w), t.c.background.ref());
            return reinterpret_cast<LRESULT>(t.brBackground);
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->code == NM_CUSTOMDRAW && L.btnInfo_.count(hdr->hwndFrom)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    L.drawButton(hdr->hwndFrom, cd->hdc, cd->rc, cd->uItemState);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (code == EN_CHANGE) {
                std::wstring typed = trim(windowText(d->edit));
                bool any = d->address ? (d->code ? looksLikeJoinTarget(typed) : looksLikeAddress(typed)) : !typed.empty();
                L.btnInfo_[d->ok].ready = any;
                InvalidateRect(d->ok, nullptr, FALSE);
                return 0;
            }
            if (id == IDOK || (code == BN_CLICKED && reinterpret_cast<HWND>(l) == d->ok)) {
                if (d->address) {
                    std::wstring typed = trim(windowText(d->edit));
                    if (!(d->code ? looksLikeJoinTarget(typed) : looksLikeAddress(typed))) {
                        SetFocus(d->edit);
                        return 0;
                    }
                    d->result = typed;
                    DestroyWindow(h);
                    return 0;
                }
                std::wstring name;
                for (wchar_t ch : windowText(d->edit))
                    if (ch != L'|' && ch != L';' && ch != L',' && ch >= 32) name += ch;
                name = trim(name);
                if (name.empty()) {
                    SetFocus(d->edit);
                    return 0;
                }
                d->result = name;
                DestroyWindow(h);
                return 0;
            }
            if (id == IDCANCEL) {
                DestroyWindow(h);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            for (auto it = L.btnInfo_.begin(); it != L.btnInfo_.end();)
                it = GetParent(it->first) == h ? L.btnInfo_.erase(it) : std::next(it);
            if (L.hoverBtn_ && GetParent(L.hoverBtn_) == h) L.hoverBtn_ = nullptr;
            d->done = true;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// One small themed window with a single text field: the player name, or a friend's address and port.
// Returns what was typed; empty when the window was closed without a value.
std::wstring Launcher::textPrompt(bool address, const std::wstring& title, const std::wstring& body,
                                  const std::wstring& labelText, const std::wstring& okText, const std::wstring& initial,
                                  bool anyTarget) {
    if (snapshot_) return {};
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = nameProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kNameClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    NameDialog d;
    d.self = this;
    d.address = address;
    d.code = anyTarget;
    HWND owner = lobbyWnd_ && IsWindowVisible(lobbyWnd_) ? lobbyWnd_ : hwnd_;
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kNameClass, title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, inst, &d);
    if (!w) return {};
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    const Theme& t = theme_;
    int pad = t.s(28), width = t.s(460), inner = width - pad * 2;
    HDC dc = GetDC(w);
    auto label = [&](const std::wstring& text, TextRole role, HFONT f, int y, int h) {
        HWND s = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX, pad, y, inner, h, w, nullptr,
                                 inst, nullptr);
        SetWindowLongPtrW(s, GWLP_USERDATA, role);
        SendMessageW(s, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        return s;
    };
    int y = t.s(26);
    label(title, kRoleText, t.title, y, t.s(36));
    y += t.s(46);
    int bodyH = textHeight(dc, t.body, body, inner);
    label(body, kRoleMuted, t.body, y, bodyH);
    y += bodyH + t.s(20);
    label(labelText, kRoleText, t.strong, y, t.s(22));
    y += t.s(28);
    int lineH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE);
    int fieldH = lineH + t.s(16);
    // A single-line edit draws its text at the top of its own window, so the dark box is painted by the
    // dialog and the edit sits inside it, centered.
    d.field = {pad, y, pad + inner, y + fieldH};
    d.edit = CreateWindowExW(0, L"EDIT", initial.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, pad + t.s(12),
                             y + (fieldH - lineH) / 2, inner - t.s(24), lineH, w, nullptr, inst, nullptr);
    SendMessageW(d.edit, WM_SETFONT, reinterpret_cast<WPARAM>(t.body), FALSE);
    SendMessageW(d.edit, EM_LIMITTEXT, address ? 260 : 20, 0);
    y += fieldH + t.s(22);
    d.ok = makeButton(IDOK + 900, BtnStyle::Primary, w);
    SetWindowTextW(d.ok, okText.c_str());
    btnInfo_[d.ok].ready = address ? (anyTarget ? looksLikeJoinTarget(trim(initial)) : looksLikeAddress(trim(initial))) : !initial.empty();
    MoveWindow(d.ok, pad + inner - t.s(150), y, t.s(150), t.s(40), FALSE);
    ShowWindow(d.ok, SW_SHOW);
    y += t.s(40) + t.s(24);
    ReleaseDC(w, dc);
    RECT rc{0, 0, width, y};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(w, GWL_STYLE), FALSE, GetWindowLongW(w, GWL_EXSTYLE), t.dpi);
    RECT o;
    GetWindowRect(owner, &o);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(w, nullptr, o.left + (o.right - o.left - ww) / 2, o.top + (o.bottom - o.top - wh) / 3, ww, wh, SWP_NOZORDER);
    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(d.edit);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return d.result;
}

bool Launcher::askAddress(std::wstring& address, const std::wstring& action) {
    std::wstring found = trim(address);
    std::wstring typed = textPrompt(true, action, cfg_.t("address.body"), cfg_.t("address.label"), cfg_.t("address.ok"),
                                    looksLikeAddress(found) ? found : std::wstring());
    if (typed.empty()) return false;
    address = typed;
    return true;
}

bool Launcher::askName(bool firstRun) {
    std::wstring name = textPrompt(false, cfg_.t("name.title"), cfg_.t("name.body"), cfg_.t("name.label"),
                                   cfg_.t("name.ok"), user_.playerName);
    if (name.empty()) return false;
    user_.playerName = name;
    save();
    logger().info(std::wstring(firstRun ? L"First run: " : L"") + L"player name set");
    if (setName_) SetWindowTextW(setName_, user_.playerName.c_str());
    if (lbRoomName_ && trim(windowText(lbRoomName_)).empty())
        SetWindowTextW(lbRoomName_, (user_.playerName + L"'s room").c_str());
    return true;
}

// ---- host and join (Ruffle Together co-op games) ----------------------------------

namespace {
constexpr wchar_t kHostClass[] = L"MultiplayerEverythingLauncherHost";
struct HostDialog {
    Launcher* self = nullptr;
    HWND players = nullptr, conn = nullptr, save = nullptr, note = nullptr, ok = nullptr, cancel = nullptr;
    std::vector<HWND> setCtl;                // one per setting shown: a checkbox or a dropdown
    std::vector<const Setting*> setDef;
    int playersSel = 0, connSel = 0, saveSel = 0;  // read when the window is accepted, before it is destroyed
    std::vector<std::string> setValues;
    bool accepted = false, done = false;
};
}  // namespace

bool Launcher::isCoopHostAction(const ActionSpec& a) {
    for (const auto& s : a.args)
        if (s.rfind(L"--coop-host", 0) == 0) return true;
    return false;
}

bool Launcher::isCoopJoinAction(const ActionSpec& a) {
    if (!a.joinArgs.empty()) return true;
    for (const auto& s : a.args)
        if (s.rfind(L"--coop-join", 0) == 0) return true;
    return false;
}

bool Launcher::coopFlow(const GameSpec& g) const {
    if (g.mod || !coopSaveGame(g)) return false;
    bool host = false, join = false;
    for (const auto& a : g.actions) {
        host = host || isCoopHostAction(a);
        join = join || isCoopJoinAction(a);
    }
    return host && join;
}

std::vector<Launcher::HostChoice> Launcher::hostChoices(const GameSpec& g) const {
    std::vector<HostChoice> out;
    for (size_t i = 0; i < g.actions.size(); ++i) {
        const ActionSpec& a = g.actions[i];
        if (!isCoopHostAction(a)) continue;
        int players = 0;
        bool relay = false;
        for (size_t k = 0; k < a.args.size(); ++k) {
            if (a.args[k] == L"--coop-host-relay") relay = true;
            if (a.args[k] == L"--coop-players" && k + 1 < a.args.size()) players = _wtoi(a.args[k + 1].c_str());
        }
        if (players > 0) out.push_back({i, players, relay});
    }
    return out;
}

LRESULT CALLBACK Launcher::hostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* d = reinterpret_cast<HostDialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        d = static_cast<HostDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(d));
    }
    if (!d) return DefWindowProcW(h, m, w, l);
    Launcher& L = *d->self;
    const Theme& t = L.theme_;
    auto accept = [&] {
        d->playersSel = d->players ? static_cast<int>(SendMessageW(d->players, CB_GETCURSEL, 0, 0)) : 0;
        d->connSel = d->conn ? static_cast<int>(SendMessageW(d->conn, CB_GETCURSEL, 0, 0)) : 0;
        d->saveSel = d->save ? static_cast<int>(SendMessageW(d->save, CB_GETCURSEL, 0, 0)) : 0;
        d->setValues.clear();
        for (size_t i = 0; i < d->setCtl.size(); ++i) {
            const Setting& s = *d->setDef[i];
            if (s.type == "checkbox") {
                d->setValues.push_back(SendMessageW(d->setCtl[i], BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0");
            } else {
                int sel = static_cast<int>(SendMessageW(d->setCtl[i], CB_GETCURSEL, 0, 0));
                d->setValues.push_back(sel >= 0 && static_cast<size_t>(sel) < s.choices.size() ? s.choices[sel].id : std::string());
            }
        }
        d->accepted = true;
        DestroyWindow(h);
    };
    switch (m) {
        case WM_ERASEBKGND: {
            RECT rc;
            GetClientRect(h, &rc);
            FillRect(reinterpret_cast<HDC>(w), &rc, t.brSurface);
            const auto& colors = L.cfg_.branding.playerColors;
            int n = static_cast<int>(colors.size());
            for (int i = 0; i < n; ++i) {
                RECT bar{rc.right * i / n, 0, rc.right * (i + 1) / n, t.s(4)};
                HBRUSH br = CreateSolidBrush(colors[i].ref());
                FillRect(reinterpret_cast<HDC>(w), &bar, br);
                DeleteObject(br);
            }
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(w);
            LONG_PTR role = GetWindowLongPtrW(reinterpret_cast<HWND>(l), GWLP_USERDATA);
            SetTextColor(dc, (role == kRoleMuted ? t.c.textMuted : role == kRoleError ? t.c.warning : t.c.text).ref());
            SetBkColor(dc, t.c.surface.ref());
            return reinterpret_cast<LRESULT>(t.brSurface);
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(l);
            if (hdr->code == NM_CUSTOMDRAW && L.btnInfo_.count(hdr->hwndFrom)) {
                auto* cd = reinterpret_cast<NMCUSTOMDRAW*>(l);
                if (cd->dwDrawStage == CDDS_PREPAINT) {
                    L.drawButton(hdr->hwndFrom, cd->hdc, cd->rc, cd->uItemState);
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            HWND from = reinterpret_cast<HWND>(l);
            if (code == CBN_SELCHANGE && from && from == d->save && d->note) {
                bool replaces = SendMessageW(d->save, CB_GETCURSEL, 0, 0) > 0;
                SetWindowLongPtrW(d->note, GWLP_USERDATA, replaces ? kRoleError : kRoleMuted);
                SetWindowTextW(d->note, replaces ? L.cfg_.t("coopSave.replaces", {{L"GAME", L.cfg_.games[L.selected_].name}}).c_str() : L"");
                InvalidateRect(d->note, nullptr, TRUE);
                return 0;
            }
            if (id == IDOK || (code == BN_CLICKED && from == d->ok)) {
                accept();
                return 0;
            }
            if (id == IDCANCEL || (code == BN_CLICKED && from == d->cancel)) {
                DestroyWindow(h);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            for (auto it = L.btnInfo_.begin(); it != L.btnInfo_.end();)
                it = GetParent(it->first) == h ? L.btnInfo_.erase(it) : std::next(it);
            if (L.hoverBtn_ && GetParent(L.hoverBtn_) == h) L.hoverBtn_ = nullptr;
            d->done = true;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// The Host window: how many players, how they connect, and the game's own settings. `action` is the
// action that starts that combination. False when the player closed it without pressing Start.
bool Launcher::hostSetup(int gi, size_t& action) {
    if (snapshot_) return false;
    const GameSpec& g = cfg_.games[gi];
    std::vector<HostChoice> choices = hostChoices(g);
    if (choices.empty()) return false;
    std::vector<int> counts;
    bool hasRelay = false, hasIp = false;
    for (const auto& c : choices) {
        if (std::find(counts.begin(), counts.end(), c.players) == counts.end()) counts.push_back(c.players);
        (c.relay ? hasRelay : hasIp) = true;
    }
    std::sort(counts.begin(), counts.end());

    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = hostProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kHostClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    HostDialog d;
    d.self = this;
    HWND owner = hwnd_;
    std::wstring title = cfg_.t("host.title", {{L"GAME", g.name}});
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, kHostClass, title.c_str(), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, inst, &d);
    if (!w) return false;
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(w, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    const Theme& t = theme_;
    int pad = t.s(28), width = t.s(460), inner = width - pad * 2;
    int comboH = t.s(30), checkH = t.s(28);
    HDC dc = GetDC(w);
    auto label = [&](const std::wstring& text, TextRole role, HFONT f, int y, int h) {
        HWND s = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX, pad, y, inner, h, w, nullptr, inst, nullptr);
        SetWindowLongPtrW(s, GWLP_USERDATA, role);
        SendMessageW(s, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        return s;
    };
    auto combo = [&](int y) {
        HWND c = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, pad, y, inner,
                                 t.s(240), w, nullptr, inst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(t.body), FALSE);
        if (theme_.dark && !theme_.highContrast) SetWindowTheme(c, L"DarkMode_CFD", nullptr);
        return c;
    };
    auto add = [&](HWND c, const std::wstring& text) { SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str())); };
    int y = t.s(26);
    label(title, kRoleText, t.title, y, t.s(36));
    y += t.s(52);
    auto row = [&](const std::wstring& text) {
        label(text, kRoleText, t.strong, y, t.s(22));
        y += t.s(26);
    };
    if (counts.size() > 1) {
        row(cfg_.t("host.players"));
        d.players = combo(y);
        for (int n : counts) add(d.players, cfg_.t("host.playersN", {{L"N", std::to_wstring(n)}}));
        SendMessageW(d.players, CB_SETCURSEL, 0, 0);
        y += comboH + t.s(14);
    }
    if (hasRelay && hasIp) {
        row(cfg_.t("host.connection"));
        d.conn = combo(y);
        add(d.conn, cfg_.t("host.viaRoom"));
        add(d.conn, cfg_.t("host.viaIp"));
        SendMessageW(d.conn, CB_SETCURSEL, 0, 0);
        y += comboH + t.s(14);
    }
    if (coopSavePlayer()) {
        row(cfg_.t("coopSave.label"));
        d.save = combo(y);
        for (const char* key : {"coopSave.continue", "coopSave.solo", "coopSave.new"}) add(d.save, cfg_.t(key));
        bool ok = coopSaveSupported();
        SendMessageW(d.save, CB_SETCURSEL, ok ? coopSaveChoiceFor(g) : 0, 0);
        EnableWindow(d.save, ok);
        y += comboH + t.s(4);
        int noteH = textHeight(dc, t.caption, L"Ag", 100, DT_SINGLELINE) * 2 + t.s(4);
        std::wstring noteText;
        LONG_PTR role = kRoleMuted;
        if (!ok) noteText = cfg_.t("coopSave.needs", {{L"VERSION", coopSavePlayer()->coopSaveSince}});
        else if (coopSaveChoiceFor(g) > 0) {
            noteText = cfg_.t("coopSave.replaces", {{L"GAME", g.name}});
            role = kRoleError;
        } else if (std::wstring f = coopSaveFile(g); !f.empty() && fs::isFile(f)) noteText = cfg_.t("coopSave.progress");
        d.note = label(noteText, kRoleMuted, t.caption, y, noteH);
        SetWindowLongPtrW(d.note, GWLP_USERDATA, role);
        y += noteH + t.s(8);
    }
    const std::vector<Setting>& list = settingsFor(g);
    int checkId = 3000;
    for (const Setting& s : list) {
        if (s.scope == "join") continue;
        bool ok = playerAtLeast(s.since);
        std::wstring text = ok ? s.label : cfg_.t("setting.needs", {{L"LABEL", s.label}, {L"VERSION", s.since}});
        std::string value = settingValue(g, s);
        HWND ctl = nullptr;
        if (s.type == "checkbox") {
            ctl = makeButton(checkId++, BtnStyle::Check, w);
            SetWindowTextW(ctl, text.c_str());
            SendMessageW(ctl, BM_SETCHECK, ok && value == "1" ? BST_CHECKED : BST_UNCHECKED, 0);
            MoveWindow(ctl, pad, y, inner, checkH, FALSE);
            ShowWindow(ctl, SW_SHOW);
            y += checkH + t.s(8);
        } else {
            row(text);
            ctl = combo(y);
            int sel = 0;
            for (size_t c = 0; c < s.choices.size(); ++c) {
                add(ctl, s.choices[c].label);
                if (s.choices[c].id == value) sel = static_cast<int>(c);
            }
            SendMessageW(ctl, CB_SETCURSEL, sel, 0);
            y += comboH + t.s(14);
        }
        EnableWindow(ctl, ok);
        d.setCtl.push_back(ctl);
        d.setDef.push_back(&s);
    }
    y += t.s(14);
    d.ok = makeButton(IDOK + 900, BtnStyle::Primary, w);
    SetWindowTextW(d.ok, cfg_.t("host.start").c_str());
    d.cancel = makeButton(IDCANCEL + 900, BtnStyle::Secondary, w);
    SetWindowTextW(d.cancel, cfg_.t("host.cancel").c_str());
    int bh = t.s(40), okW = t.s(150), cancelW = t.s(130);
    MoveWindow(d.ok, pad + inner - okW, y, okW, bh, FALSE);
    MoveWindow(d.cancel, pad + inner - okW - t.s(10) - cancelW, y, cancelW, bh, FALSE);
    ShowWindow(d.ok, SW_SHOW);
    ShowWindow(d.cancel, SW_SHOW);
    y += bh + t.s(24);
    ReleaseDC(w, dc);
    RECT rc{0, 0, width, y};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(w, GWL_STYLE), FALSE, GetWindowLongW(w, GWL_EXSTYLE), t.dpi);
    RECT o;
    GetWindowRect(owner, &o);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(w, nullptr, o.left + (o.right - o.left - ww) / 2, o.top + (o.bottom - o.top - wh) / 3, ww, wh, SWP_NOZORDER);
    EnableWindow(owner, FALSE);
    ShowWindow(w, SW_SHOW);
    SetFocus(d.ok);
    MSG msg;
    while (!d.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(w, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!d.accepted) return false;

    // What was picked becomes what the action reads: the co-op save choice and the settings.
    bool relay = d.conn ? d.connSel == 0 : hasRelay;
    int players = counts[std::clamp(d.playersSel, 0, static_cast<int>(counts.size()) - 1)];
    const HostChoice* pick = nullptr;
    for (const auto& c : choices)
        if (c.players == players && c.relay == relay) pick = &c;
    if (!pick)
        for (const auto& c : choices)
            if (c.players == players) pick = &c;
    if (!pick) return false;
    if (d.save && coopSaveSupported()) coopSaveChoice_[g.id] = std::clamp(d.saveSel, 0, 2);
    for (size_t i = 0; i < d.setDef.size(); ++i)
        if (playerAtLeast(d.setDef[i]->since) && !d.setValues[i].empty()) user_.stats(g.id).settings[d.setDef[i]->id] = d.setValues[i];
    save();
    action = pick->action;
    return true;
}

bool Launcher::coopReady(const GameSpec& g) const {
    for (const auto& a : g.actions)
        if ((isCoopHostAction(a) || isCoopJoinAction(a)) && lib_.resolve(g, a, user_).state == ActionState::Ready) return true;
    return false;
}

void Launcher::onHostGame() {
    if (selected_ < 0) return;
    if (!coopReady(cfg_.games[selected_])) {
        notice(cfg_.t("notice.needGame"), theme_.c.error);
        return;
    }
    size_t action = 0;
    if (!hostSetup(selected_, action)) return;
    runAction(selected_, action);
}

void Launcher::onJoinGame() {
    if (selected_ < 0) return;
    int gi = selected_;
    const GameSpec& g = cfg_.games[gi];
    if (!coopReady(g)) {
        notice(cfg_.t("notice.needGame"), theme_.c.error);
        return;
    }
    // The clipboard is only a guess for the box: friends usually copy the code or address first.
    std::wstring clip = clipboardText();
    std::wstring typed = textPrompt(true, cfg_.t("button.joinGame"), cfg_.t("join.body"), cfg_.t("join.label"), cfg_.t("join.ok"),
                                    looksLikeJoinTarget(clip) ? clip : std::wstring(), true);
    if (typed.empty()) return;
    if (looksLikeAddress(typed)) {
        for (size_t i = 0; i < g.actions.size(); ++i) {
            bool wants = false;
            for (const auto& arg : g.actions[i].args)
                if (arg.find(L"${ADDRESS}") != std::wstring::npos) wants = true;
            if (!wants) continue;
            pendingAddress_ = typed;
            runAction(gi, i);
            pendingAddress_.clear();
            return;
        }
    } else {
        for (auto& ch : typed) ch = static_cast<wchar_t>(towupper(ch));
        for (size_t i = 0; i < g.actions.size(); ++i)
            if (!g.actions[i].joinArgs.empty()) {
                runAction(gi, i, {{L"join", typed}});
                return;
            }
    }
    notice(cfg_.t("notice.launchFailed", {{L"PLAYER", g.name}, {L"ERROR", typed}}), theme_.c.error);
}

// ---- diagnostics --------------------------------------------------------------

void Launcher::copyDiagnostics() {
    std::wstring d = L"Multiplayer Everything Launcher " + std::wstring(LAUNCHER_VERSION) + L"\n";
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            auto value = [&](const wchar_t* name) {
                wchar_t buf[128] = {};
                DWORD size = sizeof buf;
                return RegQueryValueExW(k, name, nullptr, nullptr, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS
                           ? std::wstring(buf)
                           : std::wstring();
            };
            d += L"Windows: " + value(L"ProductName") + L" " + value(L"DisplayVersion") + L" build " + value(L"CurrentBuild") + L"\n";
            RegCloseKey(k);
        }
    }
    d += L"Config: " + cfg_.configDir + L"\n";
    d += L"Online features: " + std::wstring(user_.online ? L"on" : L"off") + L", name set: " +
         (user_.playerName.empty() ? L"no" : L"yes") + L"\n";
    {
        std::lock_guard<std::mutex> lock(installMu_);
        d += L"Server: " + (roomServer_.empty() ? cfg_.rooms.server : roomServer_) + L", ping " +
             (pingMs_ >= 0 ? std::to_wstring(pingMs_) + L" ms" : L"no answer") + L"\n";
    }
    d += L"Players:\n";
    for (const auto& p : lib_.players())
        d += L"  " + p.spec->name + L": " + (p.found() ? p.exe : L"not found") + L"\n";
    int gi = lobbyGame_ >= 0 ? lobbyGame_ : selected_;
    if (gi >= 0) {
        const GameSpec& g = cfg_.games[gi];
        d += L"Game: " + g.name + L" (" + widen(g.id) + L", server id " + widen(g.onlineId) + L")\n";
        if (!g.actions.empty()) {
            ResolvedAction r = lib_.resolve(g, g.actions[joinActionIndex(g)], user_);
            d += L"  file: " + (r.swf.empty() ? L"missing" : r.swf) + L"\n";
            if (g.lobby.enabled) d += L"  lobby version: " + widen(lobbyVersion(gi)) + L"\n";
        }
    }
    if (lobby_) {
        const lobby::Snapshot& s = lobbySnap_;
        static const wchar_t* phases[] = {L"connecting", L"lobby", L"starting", L"in game", L"closed"};
        d += L"Lobby: " + std::wstring(s.transport == lobby::Transport::Online ? L"online" : L"direct") + L", " +
             (s.host ? L"host" : L"guest") + L", " + phases[static_cast<int>(s.phase)] + L", room " + s.code + L", slot " +
             std::to_wstring(s.mySlot) + L", bridge port " + std::to_wstring(s.bridgePort) + L", game connected " +
             (s.gameConnected ? L"yes" : L"no") + (s.serverV2 ? L"" : L", older server") + L"\n";
        for (const auto& p : s.players)
            d += L"  P" + std::to_wstring(p.slot + 1) + L" " + (p.connected ? L"" : L"(away) ") + (p.ready ? L"ready " : L"") +
                 (p.pingMs >= 0 ? std::to_wstring(p.pingMs) + L" ms" : L"") + L"\n";
    }
    d += L"Network:\n";
    for (const auto& a : net::shareAddresses(sys::localAddresses()))
        d += L"  " + a.address + L"  " + a.adapter + (a.vpn.empty() ? L"" : L" (" + a.vpn + L")") + L"\n";
    if (!lobbyNetText_.empty()) d += lobbyNetText_ + L"\n";
    d += L"\nLog (latest lines):\n";
    uint64_t next = 0;
    std::vector<std::wstring> lines = logger().since(0, next);
    size_t from = lines.size() > 200 ? lines.size() - 200 : 0;
    for (size_t i = from; i < lines.size(); ++i) d += lines[i] + L"\n";
    // Your name stays out of it: it's only the player's to share.
    if (!user_.playerName.empty()) d = replaceAll(d, user_.playerName, L"<name>");
    copyText(d);
}

}  // namespace fl::ui
