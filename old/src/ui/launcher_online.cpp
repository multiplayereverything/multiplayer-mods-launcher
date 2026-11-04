// The launcher window: rooms, the server status, toasts, Discord, and updating the launcher itself.
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

// ---- online play ------------------------------------------------------------

bool Launcher::actionShown(const ActionSpec& a) const {
    return (!a.original || user_.showOriginals) && (!a.offline || user_.showOffline);
}

bool Launcher::notOnServer(const GameSpec& g) const { return onlineOn_ && roomsKnown_ && rejected_.count(g.id) > 0; }

bool Launcher::roomsPage() const { return !navItems_.empty() && navItems_[navIndex_].key == "rooms"; }

size_t Launcher::joinActionIndex(const GameSpec& g) const {
    for (size_t i = 0; i < g.actions.size(); ++i)
        if (g.actions[i].id == g.joinAction) return i;
    return 0;
}

int Launcher::playersOnline() const {
    if (!onlineOn_ || !roomsKnown_) return 0;
    int n = 0;
    for (const auto& [id, list] : rooms_)
        for (const auto& r : list) n += r.players;
    return n;
}

void Launcher::refreshRoomsNav() {
    for (size_t i = 0; i < navItems_.size(); ++i) {
        if (navItems_[i].key != "rooms") continue;
        navItems_[i].count = playersOnline();
        // Screen readers read the item's text, so it carries the count too.
        std::wstring text = navItems_[i].label + L" (" +
                            cfg_.t("nav.online", {{L"N", std::to_wstring(navItems_[i].count)}}) + L")";
        SendMessageW(nav_, LB_DELETESTRING, i, 0);
        SendMessageW(nav_, LB_INSERTSTRING, i, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
        int sel = static_cast<int>(SendMessageW(navCombo_, CB_GETCURSEL, 0, 0));
        SendMessageW(navCombo_, CB_DELETESTRING, i, 0);
        SendMessageW(navCombo_, CB_INSERTSTRING, i, reinterpret_cast<LPARAM>(text.c_str()));
        SendMessageW(navCombo_, CB_SETCURSEL, sel, 0);
    }
    syncChips();
}

std::wstring Launcher::serverText(Color& color) const {
    const Theme& t = theme_;
    bool any = std::any_of(cfg_.games.begin(), cfg_.games.end(), [](const GameSpec& g) { return !g.onlineId.empty(); });
    if (!any) return {};
    if (!onlineOn_) {
        color = t.c.sidebarTextMuted;
        return cfg_.t("server.off");
    }
    if (!serverAsked_) {
        color = t.c.sidebarTextMuted;
        return cfg_.t("server.checking");
    }
    if (!roomsKnown_) {
        color = t.c.warning;
        return cfg_.t("server.down");
    }
    color = t.c.success;
    if (pingMs_ < 0) return cfg_.t("server.okUntimed");
    return cfg_.t("server.ok", {{L"MS", std::to_wstring(std::max(pingMs_, 1))}});
}

std::vector<Launcher::RoomRow> Launcher::listedRooms(int onlyGame, bool hideFull) const {
    std::vector<RoomRow> out;
    if (!onlineOn_ || !roomsKnown_) return out;
    std::vector<std::wstring> words = split(toLower(trim(windowText(search_))), L' ');
    for (size_t gi = 0; gi < cfg_.games.size(); ++gi) {
        if (onlyGame >= 0 && static_cast<int>(gi) != onlyGame) continue;
        auto it = rooms_.find(cfg_.games[gi].id);
        if (it == rooms_.end()) continue;
        for (const auto& r : it->second) {
            bool full = r.max > 0 && r.players >= r.max;
            if (hideFull && full) continue;
            // The search box filters rooms too: game, code or room name.
            std::wstring hay = toLower(cfg_.games[gi].name + L" " + r.code + L" " + r.name);
            bool hit = true;
            for (const auto& w : words) hit = hit && (w.empty() || hay.find(w) != std::wstring::npos);
            if (hit) out.push_back({static_cast<int>(gi), r, {}});
        }
    }
    // Busiest first: that's where a match is about to start.
    std::stable_sort(out.begin(), out.end(), [](const RoomRow& a, const RoomRow& b) { return a.room.players > b.room.players; });
    return out;
}

void Launcher::layoutRoomsPage() {
    std::vector<HWND> controls = {pageGameCombo_, pageHideFull_, pageCodeEdit_, pageCodeBtn_};
    controls.insert(controls.end(), pageRoomBtns_.begin(), pageRoomBtns_.end());
    pageRows_.clear();
    if (!pageGameCombo_) return;
    if (!roomsPage() || width(gridRc_) <= 0) {
        for (HWND h : controls) ShowWindow(h, SW_HIDE);
        pageList_ = pageCodeBox_ = {};
        return;
    }
    std::set<HWND> shown = {pageGameCombo_, pageHideFull_, pageCodeEdit_, pageCodeBtn_};
    const Theme& t = theme_;
    HDC dc = GetDC(hwnd_);
    int x = gridRc_.left + t.s(kPad), w = width(gridRc_) - t.s(kPad) * 2, y = gridRc_.top + t.s(4);
    int headH = textHeight(dc, t.heading, L"Ag", 100, DT_SINGLELINE);
    pageTitle_ = {x, y, x + w, y + headH};
    y += headH + t.s(4);
    std::wstring subtitle = cfg_.t("roomsPage.subtitle");
    int subH = textHeight(dc, t.caption, subtitle, w);
    pageSub_ = {x, y, x + w, y + subH};
    y += subH + t.s(16);

    // Toolbar: game filter and "Hide full rooms" on the left, join by code on the right.
    int barH = t.s(34);
    int comboW = std::min(t.s(200), w / 3);
    MoveWindow(pageGameCombo_, x, y + (barH - t.s(30)) / 2, comboW, t.s(300), TRUE);
    int checkW = textWidth(dc, t.body, cfg_.t("roomsPage.hideFull")) + t.s(36);
    MoveWindow(pageHideFull_, x + comboW + t.s(14), y, checkW, barH, TRUE);
    int jbw = textWidth(dc, t.button, cfg_.t("roomsPage.joinCode")) + t.s(32), codeW = t.s(120);
    int lineH = textHeight(dc, t.button, L"Ag", 100, DT_SINGLELINE);
    if (x + comboW + checkW + jbw + codeW + t.s(40) > x + w) {  // narrow: the code row moves under
        y += barH + t.s(10);
        pageCodeBox_ = {x, y, x + codeW, y + barH};
        MoveWindow(pageCodeBtn_, x + codeW + t.s(8), y, jbw, barH, TRUE);
    } else {
        pageCodeBox_ = {x + w - jbw - t.s(8) - codeW, y, x + w - jbw - t.s(8), y + barH};
        MoveWindow(pageCodeBtn_, x + w - jbw, y, jbw, barH, TRUE);
    }
    MoveWindow(pageCodeEdit_, pageCodeBox_.left + t.s(12), pageCodeBox_.top + (barH - lineH) / 2,
               width(pageCodeBox_) - t.s(20), lineH, TRUE);
    y += barH + t.s(16);

    pageList_ = {x, y, x + w, gridRc_.bottom - t.s(kPad)};
    int sel = static_cast<int>(SendMessageW(pageGameCombo_, CB_GETCURSEL, 0, 0));
    int onlyGame = sel > 0 ? static_cast<int>(SendMessageW(pageGameCombo_, CB_GETITEMDATA, sel, 0)) : -1;
    bool hideFull = SendMessageW(pageHideFull_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::vector<RoomRow> rows = listedRooms(onlyGame, hideFull);
    int rowH = t.s(48);
    int fit = std::max(1, height(pageList_) / rowH);
    pageScroll_ = std::clamp(pageScroll_, 0, std::max(0, static_cast<int>(rows.size()) - fit));
    size_t first = static_cast<size_t>(pageScroll_);
    size_t n = std::min<size_t>(rows.size() - std::min(first, rows.size()), static_cast<size_t>(fit));
    n = std::min<size_t>(n, kIdPageRoomEnd - kIdPageRoom0);
    while (pageRoomBtns_.size() < n) {
        HWND b = makeButton(kIdPageRoom0 + static_cast<int>(pageRoomBtns_.size()), BtnStyle::Secondary);
        btnInfo_[b].label = cfg_.t("rooms.join");
        pageRoomBtns_.push_back(b);
        controls.push_back(b);
    }
    int bw = t.s(72);
    for (size_t i = 0; i < n; ++i) {
        RoomRow row = rows[first + i];
        row.rc = {x, y + static_cast<int>(i) * rowH, x + w, y + static_cast<int>(i + 1) * rowH};
        pageRows_.push_back(row);
        HWND b = pageRoomBtns_[i];
        MoveWindow(b, row.rc.right - bw, row.rc.top + t.s(7), bw, rowH - t.s(14), TRUE);
        SetWindowTextW(b, cfg_.t("rooms.joinName", {{L"CODE", row.room.code}}).c_str());
        EnableWindow(b, row.room.max <= 0 || row.room.players < row.room.max);
        shown.insert(b);
    }
    ReleaseDC(hwnd_, dc);
    for (HWND h : controls) ShowWindow(h, shown.count(h) ? SW_SHOW : SW_HIDE);
    for (HWND h : controls) InvalidateRect(h, nullptr, FALSE);
}

void Launcher::paintRoomRow(Graphics& g, const RoomRow& row, bool showGame, int joinW) {
    const Theme& t = theme_;
    const RECT& r = row.rc;
    Pen line(gc(t.c.border), 1);
    g.DrawLine(&line, static_cast<REAL>(r.left), static_cast<REAL>(r.top), static_cast<REAL>(r.right),
               static_cast<REAL>(r.top));
    int x = r.left, mid = (r.top + r.bottom) / 2;
    if (showGame && row.game >= 0) {
        const GameSpec& game = cfg_.games[row.game];
        int chip = t.s(12);
        fillRound(g, RectF(static_cast<REAL>(x), static_cast<REAL>(mid - chip / 2), static_cast<REAL>(chip),
                           static_cast<REAL>(chip)),
                  t.sf(3), game.color);
        RECT gr{x + chip + t.s(10), r.top, x + t.s(200), r.bottom};
        drawText(g, game.name, gr, t.body, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        x += t.s(212);
    }
    // The code decodes the first time it shows up.
    std::wstring code = row.room.code;
    if (auto c = codeFrom_.find(code); c != codeFrom_.end())
        if (ULONGLONG e = since(c->second); e < kCodeMs)
            code = motion::scramble(code, static_cast<float>(e) / kCodeMs, static_cast<uint32_t>(e / 50));
    RECT cr{x, r.top, x + t.s(72), r.bottom};
    drawText(g, code, cr, t.button, t.c.text, DT_SINGLELINE | DT_VCENTER);
    x += t.s(78);

    // Players: one block per slot, filled in the players' colors (P1-P4); bigger
    // rooms as "3/8".
    int right = r.right - joinW - t.s(12);
    const auto& colors = cfg_.branding.playerColors;
    const RoomInfo& room = row.room;
    if (room.max > 0 && room.max <= 4 && !colors.empty()) {
        int pip = t.s(10), pipGap = t.s(4);
        int px = right - room.max * (pip + pipGap) + pipGap;
        for (int i = 0; i < room.max; ++i, px += pip + pipGap) {
            RectF pr(static_cast<REAL>(px), static_cast<REAL>(mid - pip / 2), static_cast<REAL>(pip), static_cast<REAL>(pip));
            if (i < room.players) fillRound(g, pr, t.sf(2), colors[i % colors.size()]);
            else strokeRound(g, pr, t.sf(2), t.c.textMuted, t.sf(1));
        }
        right -= room.max * (pip + pipGap) + t.s(10);
    } else {
        std::wstring n = cfg_.t("rooms.players", {{L"N", std::to_wstring(room.players)}, {L"MAX", std::to_wstring(room.max)}});
        RECT nr{right - t.s(48), r.top, right, r.bottom};
        drawText(g, n, nr, t.caption, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        right -= t.s(56);
    }
    // Lobby rooms say whether they're playing already or need a password.
    std::wstring about = room.name;
    if (room.inGame()) about += (about.empty() ? L"" : L", ") + cfg_.t("rooms.playing");
    if (room.locked) about += (about.empty() ? L"" : L", ") + cfg_.t("rooms.locked");
    if (!about.empty() && right > x) {
        RECT nr{x, r.top, right, r.bottom};
        drawText(g, about, nr, t.caption, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

void Launcher::paintRoomsPage(Graphics& g) {
    const Theme& t = theme_;
    drawText(g, cfg_.t("roomsPage.title"), pageTitle_, t.heading, t.c.text, DT_SINGLELINE);
    drawText(g, cfg_.t("roomsPage.subtitle"), pageSub_, t.caption, t.c.textMuted, DT_WORDBREAK);
    if (width(pageCodeBox_) > 0) {
        fillRound(g, rf(pageCodeBox_), t.sf(6), t.c.surface);
        strokeRound(g, rf(pageCodeBox_), t.sf(6), GetFocus() == pageCodeEdit_ ? t.c.focus : t.c.border, t.sf(1));
    }
    for (const auto& row : pageRows_) paintRoomRow(g, row, true, t.s(72));
    if (pageRows_.empty() && height(pageList_) > 0) {
        const char* head = !onlineOn_ ? "roomsPage.disabled" : !roomsKnown_ ? "roomsPage.offline" : "roomsPage.empty";
        const char* body = !onlineOn_    ? "roomsPage.disabledBody"
                           : !roomsKnown_ ? "roomsPage.offlineBody"
                                          : "roomsPage.emptyBody";
        RECT r = pageList_;
        r.top += t.s(40);
        drawText(g, cfg_.t(head), r, t.heading, t.c.text, DT_CENTER | DT_SINGLELINE);
        r.top += t.s(32);
        drawText(g, cfg_.t(body), r, t.body, t.c.textMuted, DT_CENTER | DT_WORDBREAK);
    }
}

void Launcher::joinListedRoom(int gi, const RoomInfo& room) {
    if (gi < 0 || gi >= static_cast<int>(cfg_.games.size())) return;
    if (room.max > 0 && room.players >= room.max) {
        notice(cfg_.t("rooms.full", {{L"CODE", room.code}}), theme_.c.warning);
        return;
    }
    const GameSpec& g = cfg_.games[gi];
    if (usesLobby(gi)) {
        openLobby(gi);
        if (lobby_ && lobby_->running()) return;
        lobbyJoin(room.code, L"", false);
        return;
    }
    copyText(room.code);
    if (g.mod) {
        // The mod has no way to take a code from the launcher yet: the player pastes it in the game.
        const ModView* v = modView(gi);
        mod::Spec spec = modSpec(gi);
        Vars vars = mod::withSpec(lib_.vars(), spec);
        vars.set(L"CODE", room.code);
        if (!v || !v->status.installed) {
            notice(spec.t("mod.joinNeedsMod", vars), theme_.c.warning);
            return;
        }
        for (const auto& a : g.actions)
            if (a.modOp == "launch") {
                if (runModAction(gi, a)) toast(spec.t("mod.joinCopied", vars), theme_.c.success);
                return;
            }
        return;
    }
    // The code goes to the game too: a mod that reads it joins by itself; with
    // one that doesn't, the player pastes it into the F9 panel.
    if (runAction(gi, joinActionIndex(g), {{L"join", room.code}}))
        toast(cfg_.t("rooms.copied", {{L"CODE", room.code}}), theme_.c.success);
}

void Launcher::joinByCode(HWND edit, int gi) {
    std::wstring code;
    for (wchar_t ch : windowText(edit))
        if (iswalnum(ch)) code += static_cast<wchar_t>(towupper(ch));
    if (code.size() != 5) {
        notice(cfg_.t("roomsPage.badCode"), theme_.c.warning);
        SetFocus(edit);
        return;
    }
    // Which game: the one asked for, else the listed room with that code, else
    // the Rooms page's game filter.
    if (gi < 0)
        for (const auto& [id, list] : rooms_)
            for (const auto& r : list)
                if (r.code == code)
                    for (size_t i = 0; i < cfg_.games.size(); ++i)
                        if (cfg_.games[i].id == id) gi = static_cast<int>(i);
    if (gi < 0) {
        int sel = static_cast<int>(SendMessageW(pageGameCombo_, CB_GETCURSEL, 0, 0));
        if (sel > 0) gi = static_cast<int>(SendMessageW(pageGameCombo_, CB_GETITEMDATA, sel, 0));
    }
    if (gi < 0) {
        notice(cfg_.t("roomsPage.pickGame", {{L"CODE", code}}), theme_.c.warning);
        SetFocus(pageGameCombo_);
        return;
    }
    if (!joinReady(cfg_.games[gi])) {
        notice(cfg_.t("notice.needGame"), theme_.c.error);
        return;
    }
    RoomInfo room;
    room.code = code;
    joinListedRoom(gi, room);
    SetWindowTextW(edit, L"");
}

bool Launcher::joinReady(const GameSpec& g) const {
    if (g.joinAction.empty() || g.actions.empty()) return true;
    const ActionSpec& a = g.actions[joinActionIndex(g)];
    if (a.isMod() || a.isLink()) return true;  // PC mods and the lobby check their own requirements
    return lib_.resolve(g, a, user_).state == ActionState::Ready;
}

void Launcher::quickPlay() {
    if (selected_ < 0) return;
    if (!joinReady(cfg_.games[selected_])) {
        notice(cfg_.t("notice.needGame"), theme_.c.error);
        return;
    }
    if (usesLobby(selected_)) {
        lobbyQuick(selected_);
        return;
    }
    const GameSpec& g = cfg_.games[selected_];
    const RoomInfo* busiest = nullptr;
    if (auto it = rooms_.find(g.id); it != rooms_.end())
        for (const auto& r : it->second)
            if ((r.max <= 0 || r.players < r.max) && (!busiest || r.players > busiest->players)) busiest = &r;
    if (busiest) {
        joinListedRoom(selected_, *busiest);
        return;
    }
    // Nobody is playing: start the game ready to host ("host" for mods that read it).
    runAction(selected_, joinActionIndex(g), {{L"host", L"1"}});
}

void Launcher::toast(const std::wstring& text, const Color& color) {
    logger().info(L"Toast: " + text);
    if (!toast_ || snapshot_) {
        notice(text, color);
        return;
    }
    toastText_ = text;
    toastColor_ = color;
    toastFrom_ = GetTickCount64();
    SetWindowTextW(toast_, text.c_str());  // screen readers read the window's name
    layoutToast();
    SetWindowPos(toast_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    NotifyWinEvent(EVENT_SYSTEM_ALERT, toast_, OBJID_CLIENT, CHILDID_SELF);
    InvalidateRect(toast_, nullptr, FALSE);
    wake();
}

void Launcher::layoutToast() {
    if (!toast_) return;
    if (!toastFrom_) {
        ShowWindow(toast_, SW_HIDE);
        return;
    }
    const Theme& t = theme_;
    ULONGLONG e = since(toastFrom_);
    if (motion_ && e > kToastMs + 250) {
        toastFrom_ = 0;
        ShowWindow(toast_, SW_HIDE);
        return;
    }
    // In over 0.18 s, rising a little; out over 0.25 s at the end.
    float in = motion_ ? motion::easeOutCubic(static_cast<float>(e) / 180.0f) : 1.0f;
    float out = motion_ && e > kToastMs ? 1.0f - static_cast<float>(e - kToastMs) / 250.0f : 1.0f;
    float alpha = std::clamp(std::min(in, out), 0.0f, 1.0f);
    const RECT& area = gridRc_;
    HDC dc = GetDC(hwnd_);
    int maxW = std::min(t.s(440), width(area) - t.s(40));
    int textW = std::min(textWidth(dc, t.body, toastText_), maxW - t.s(48));
    int textH = textHeight(dc, t.body, toastText_, std::max(textW, 1));
    ReleaseDC(hwnd_, dc);
    int w = textW + t.s(48), h = textH + t.s(24);
    int x = area.left + (width(area) - w) / 2;
    int y = area.bottom - t.s(20) - h + static_cast<int>(t.s(10) * (1 - in));
    SetWindowPos(toast_, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
    SetLayeredWindowAttributes(toast_, 0, static_cast<BYTE>(255 * alpha), LWA_ALPHA);
}

void Launcher::paintToast(HDC hdc) {
    const Theme& t = theme_;
    RECT r;
    GetClientRect(toast_, &r);
    Buffer buf(hdc, width(r), height(r));
    {
        Graphics g(buf.dc);
        SolidBrush under(gc(t.c.background));
        g.FillRectangle(&under, 0, 0, width(r), height(r));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        RectF box(0.5f, 0.5f, static_cast<REAL>(width(r)) - 1, static_cast<REAL>(height(r)) - 1);
        fillRound(g, box, t.sf(6), t.c.surfaceHover);
        strokeRound(g, box, t.sf(6), mix(t.c.surfaceHover, toastColor_, 0.6f), t.sf(1));
        SolidBrush dot(gc(toastColor_));
        g.FillEllipse(&dot, t.sf(16), static_cast<REAL>(height(r)) / 2 - t.sf(4), t.sf(8), t.sf(8));
        RECT tr{t.s(32), t.s(12), width(r) - t.s(16), height(r) - t.s(12)};
        drawText(g, toastText_, tr, t.body, t.c.text, DT_WORDBREAK);
    }
    BitBlt(hdc, 0, 0, width(r), height(r), buf.dc, 0, 0, SRCCOPY);
}

LRESULT CALLBACK Launcher::toastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self && m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        self->paintToast(dc);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_LBUTTONDOWN && self) {  // a click dismisses it
        self->toastFrom_ = 0;
        ShowWindow(h, SW_HIDE);
        return 0;
    }
    if (m == WM_NCHITTEST) return HTCLIENT;
    return DefWindowProcW(h, m, w, l);
}

void Launcher::updatePresence() {
    if (!discord_) return;
    if (sessions_.empty()) {
        discord_->clear();
        return;
    }
    const Session& s = sessions_.back();
    Presence p;
    p.details = cfg_.t("discord.playing", {{L"GAME", cfg_.games[s.game].name}});
    if (!s.room.empty()) {
        p.state = cfg_.t("discord.room", {{L"CODE", s.room}});
    } else {
        for (const auto& a : cfg_.games[s.game].actions)
            if (a.id == s.action) p.state = a.label;
    }
    p.start = s.startedUnix;
    discord_->set(p);
}

bool Launcher::stepLift() {
    ULONGLONG now = GetTickCount64();
    float dt = liftTick_ ? static_cast<float>(now - liftTick_) / 140.0f : 0.0f;  // 140 ms all the way
    liftTick_ = now;
    bool any = false;
    int hovered = hoverItem_ >= 0 && hoverItem_ < static_cast<int>(visible_.size()) ? visible_[hoverItem_] : -1;
    if (hovered >= 0 && !lift_.count(hovered)) lift_[hovered] = 0.0f;
    for (auto it = lift_.begin(); it != lift_.end();) {
        float target = it->first == hovered ? 1.0f : 0.0f;
        float v = it->second;
        v = target > v ? std::min(target, v + dt) : std::max(target, v - dt);
        bool moved = v != it->second;
        it->second = v;
        if (moved) {
            any = true;
            auto pos = std::find(visible_.begin(), visible_.end(), it->first);
            RECT r;
            if (pos != visible_.end() &&
                ListView_GetItemRect(grid_, static_cast<int>(pos - visible_.begin()), &r, LVIR_BOUNDS))
                InvalidateRect(grid_, &r, FALSE);
        }
        if (v <= 0.0f && it->first != hovered) it = lift_.erase(it);
        else ++it;
    }
    return any;
}

bool Launcher::anyBusy() const {
    for (const auto& [b, info] : btnInfo_)
        if (info.busy && IsWindowVisible(b)) return true;
    return false;
}

// ---- updating the launcher itself --------------------------------------------

void Launcher::startUpdateCheck() {
    removeOldUpdate(fs::selfExePath());  // what the last update set aside
    if (!user_.autoUpdate || !user_.online) return;
    HWND wnd = hwnd_;
    bool selfUpdate = cfg_.updates.enabled();
    InstallSpec source = cfg_.updates.source;
    std::vector<PlayerSpec> players = cfg_.players;
    Vars vars = lib_.vars();
    // Not installed yet, the app that replaces this window (main.cpp opens it once it is): its installer comes
    // from the same releases. Afterwards this window stays as it is; the app updates itself.
    std::wstring appInstaller;
    if (selfUpdate && cfg_.updates.handsOver() && !fs::isFile(fs::normalize(vars.expand(cfg_.updates.appExe))))
        appInstaller = fs::join(lib_.dataDir(), L"MultiplayerEverything-setup.exe");
    std::wstring appPattern = cfg_.updates.app;
    updateThread_ = std::thread([wnd, selfUpdate, source, players, vars, appInstaller, appPattern, this] {
        if (!appInstaller.empty()) {
            UpdateResult r = downloadApp(source, appPattern, appInstaller, &installCancel_);
            if (r.ok) {
                logger().info(L"Downloaded the app's installer (" + r.version + L")");
                PostMessageW(wnd, WM_APP_HANDOVER, 0, reinterpret_cast<LPARAM>(new std::wstring(appInstaller)));
                return;
            }
            logger().warn(L"The app's installer: " + r.error);
        }
        if (selfUpdate) {
            UpdateResult r = updateSelf(source, LAUNCHER_VERSION, fs::selfExePath(), &installCancel_);
            if (!r.ok) {
                logger().warn(L"Update check: " + r.error);
                if (isRateLimitMessage(r.error)) {
                    PostMessageW(wnd, WM_APP_RATELIMIT, 0, reinterpret_cast<LPARAM>(new std::wstring(r.error)));
                    return;  // the player check would only meet the same limit
                }
            }
            else if (!r.updated) logger().info(L"Launcher is up to date (newest release " + r.version + L")");
            else {
                logger().info(L"Installed launcher " + r.version + L"; it runs from the next start");
                PostMessageW(wnd, WM_APP_UPDATED, 0, reinterpret_cast<LPARAM>(new std::wstring(r.version)));
                return;  // the launcher restarts (or starts new next time) and checks the player then
            }
        }
        // A player the launcher installed from a release (Ruffle Together) is kept on the newest one: everyone
        // in a co-op session has to run the same build.
        for (const auto& p : players) {
            PlayerUpdate u = updateInstalledPlayer(p, vars, &installCancel_);
            if (!u.error.empty()) {
                logger().warn(p.name + L" update check: " + u.error);
                if (isRateLimitMessage(u.error)) {
                    PostMessageW(wnd, WM_APP_RATELIMIT, 0, reinterpret_cast<LPARAM>(new std::wstring(u.error)));
                    return;
                }
            }
            else if (u.updated)
                PostMessageW(wnd, WM_APP_PLAYERUPDATED, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(p.name + L" " + u.version)));
            else if (u.checked)
                logger().info(p.name + L" is up to date (newest release " + u.version + L")");
        }
    });
}

void Launcher::onPlayerUpdated(const std::wstring& what) {
    lib_.refresh();
    toast(cfg_.t("update.player", {{L"PLAYER", what}}), theme_.c.success);
    select(selected_);
}

void Launcher::onHandover(const std::wstring& installer) {
    // The installer opens the app when it is done, and from then on this launcher only opens the app (main.cpp).
    // While a game runs or a room is open the window stays; the installer runs at the next start.
    bool busy = !sessions_.empty() || relay_.running() || installing_ || (lobby_ && lobby_->running()) || settingsWnd_ ||
                (lobbyWnd_ && IsWindowVisible(lobbyWnd_));
    if (busy) {
        toast(cfg_.t("update.appNextTime"), theme_.c.success);
        return;
    }
    save();
    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(installer, {}, fs::parent(installer), p, err)) {
        notice(err, theme_.c.error);
        return;
    }
    CloseHandle(p.process);
    logger().info(L"Started " + installer + L"; this window closes");
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void Launcher::onUpdated(const std::wstring& version) {
    // The new version is in place. Restart into it at once, without asking, unless the launcher is in the
    // middle of something (a game runs, a room or server is open, a download or install, Settings): then it
    // simply starts with the new version next time.
    bool busy = !sessions_.empty() || relay_.running() || installing_ || (lobby_ && lobby_->running()) || settingsWnd_ ||
                (lobbyWnd_ && IsWindowVisible(lobbyWnd_));
    if (busy) {
        toast(cfg_.t("update.nextTime", {{L"VERSION", version}}), theme_.c.success);
        return;
    }
    save();
    sys::Started p;
    std::wstring err;
    if (!sys::startProcess(fs::selfExePath(), {}, fs::parent(fs::selfExePath()), p, err)) {
        notice(err, theme_.c.error);
        return;
    }
    CloseHandle(p.process);
    logger().info(L"Restarting into launcher " + version);
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

}  // namespace fl::ui
