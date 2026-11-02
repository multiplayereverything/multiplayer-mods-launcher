// The Settings window: a small modal window owned by the launcher. Every change
// applies and saves at once, so there is no Save/Cancel pair to get wrong.
#include "launcher.h"

#include "core/fs.h"
#include "dialogs.h"
#include "core/log.h"
#include "core/strings.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>

namespace fl::ui {

namespace {

constexpr wchar_t kSettingsClass[] = L"MultiplayerEverythingLauncherSettings";

enum : int {
    kSetMinimize = 500,
    kSetPlayer,
    kSetOnline,
    kSetChange,
    kSetOpen,
    kSetReset,
    kSetLog,
    kSetWeb,
    kSetClose,
    kSetOriginals,
    kSetName,
    kSetAutoUpdate,
    kSetOffline,
};

// Static text roles, kept in the control's user data so WM_CTLCOLORSTATIC knows
// which color to use.
enum TextRole : LONG_PTR { kRoleText = 1, kRoleMuted, kRoleHeading };

}  // namespace

void Launcher::openSettings() {
    if (snapshot_) return;
    if (settingsWnd_) {
        SetForegroundWindow(settingsWnd_);
        return;
    }
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = settingsProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kSettingsClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    std::wstring title = cfg_.t("settings.title");
    settingsWnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kSettingsClass, title.c_str(),
                                   WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                   100, 100, hwnd_, nullptr, inst, this);
    if (!settingsWnd_) return;
    BOOL darkTitle = theme_.dark ? TRUE : FALSE;
    DwmSetWindowAttribute(settingsWnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitle, sizeof darkTitle);
    HWND w = settingsWnd_;
    auto label = [&](const std::wstring& text, TextRole role, DWORD extra = 0) {
        HWND h = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX | extra, 0, 0, 10, 10, w,
                                 nullptr, inst, nullptr);
        SetWindowLongPtrW(h, GWLP_USERDATA, role);
        SendMessageW(h, WM_SETFONT,
                     reinterpret_cast<WPARAM>(role == kRoleHeading ? theme_.heading
                                              : role == kRoleMuted ? theme_.caption
                                                                   : theme_.body),
                     FALSE);
        setLabels_.push_back(h);
        return h;
    };
    auto button = [&](int id, BtnStyle style, const std::wstring& text) {
        HWND b = makeButton(id, style, w);
        SetWindowTextW(b, text.c_str());
        ShowWindow(b, SW_SHOW);
        return b;
    };

    // Creation order is the Tab order: top to bottom, left to right.
    setLabels_.clear();
    label(cfg_.t("settings.playing"), kRoleHeading);
    setMinimize_ = button(kSetMinimize, BtnStyle::Check, cfg_.t("settings.minimize"));
    SendMessageW(setMinimize_, BM_SETCHECK, minimizeOnLaunch() ? BST_CHECKED : BST_UNCHECKED, 0);
    label(cfg_.t("settings.player"), kRoleText);  // the combo's accessible name comes from this label
    setPlayer_ = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 10, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetPlayer)), inst, nullptr);
    SendMessageW(setPlayer_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), FALSE);
    if (theme_.dark && !theme_.highContrast) SetWindowTheme(setPlayer_, L"DarkMode_CFD", nullptr);
    std::wstring autoText = cfg_.t("detail.playerAutoNone");
    SendMessageW(setPlayer_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(autoText.c_str()));
    int sel = 0;
    for (size_t i = 0; i < cfg_.players.size(); ++i) {
        const PlayerStatus* ps = lib_.player(cfg_.players[i].id);
        std::wstring text = cfg_.players[i].name + (ps && ps->found() ? L"" : L"  (not found)");
        SendMessageW(setPlayer_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        if (cfg_.players[i].id == user_.defaultPlayer) sel = static_cast<int>(i) + 1;
    }
    SendMessageW(setPlayer_, CB_SETCURSEL, sel, 0);
    setOffline_ = button(kSetOffline, BtnStyle::Check, cfg_.t("settings.offline"));
    SendMessageW(setOffline_, BM_SETCHECK, user_.showOffline ? BST_CHECKED : BST_UNCHECKED, 0);
    setOriginals_ = button(kSetOriginals, BtnStyle::Check, cfg_.t("settings.originals"));
    SendMessageW(setOriginals_, BM_SETCHECK, user_.showOriginals ? BST_CHECKED : BST_UNCHECKED, 0);

    label(cfg_.t("settings.online"), kRoleHeading);
    setOnline_ = button(kSetOnline, BtnStyle::Check, cfg_.t("settings.onlineToggle"));
    SendMessageW(setOnline_, BM_SETCHECK, user_.online ? BST_CHECKED : BST_UNCHECKED, 0);
    label(cfg_.t("settings.onlineNote"), kRoleMuted);
    setAutoUpdate_ = button(kSetAutoUpdate, BtnStyle::Check, cfg_.t("settings.autoUpdate"));
    SendMessageW(setAutoUpdate_, BM_SETCHECK, user_.autoUpdate ? BST_CHECKED : BST_UNCHECKED, 0);
    label(cfg_.t("settings.name"), kRoleText);  // the field's accessible name comes from this label
    setName_ = CreateWindowExW(0, L"EDIT", user_.playerName.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                               10, 10, w, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSetName)), inst, nullptr);
    SendMessageW(setName_, WM_SETFONT, reinterpret_cast<WPARAM>(theme_.body), FALSE);
    SendMessageW(setName_, EM_LIMITTEXT, 20, 0);
    label(cfg_.t("settings.nameNote"), kRoleMuted);

    label(cfg_.t("settings.folders"), kRoleHeading);
    label(cfg_.t("settings.gamesDir"), kRoleText);
    setDirText_ = label(lib_.vars().get(L"GAMES_DIR"), kRoleMuted, SS_PATHELLIPSIS);
    button(kSetChange, BtnStyle::Secondary, cfg_.t("settings.change"));
    button(kSetOpen, BtnStyle::Secondary, cfg_.t("settings.open"));
    setReset_ = button(kSetReset, BtnStyle::Ghost, cfg_.t("settings.reset"));
    button(kSetLog, BtnStyle::Secondary, cfg_.t("settings.log"));
    label(cfg_.t("settings.about", {{L"VERSION", LAUNCHER_VERSION}}), kRoleMuted);
    if (!cfg_.branding.website.empty()) button(kSetWeb, BtnStyle::Ghost, cfg_.t("settings.github"));
    button(kSetClose, BtnStyle::Secondary, cfg_.t("settings.close"));

    layoutSettings();
    EnableWindow(hwnd_, FALSE);  // modal: the library waits until Settings closes
    ShowWindow(settingsWnd_, SW_SHOW);
    SetFocus(setMinimize_);
    MSG msg;
    while (settingsWnd_ && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(settingsWnd_, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Launcher::layoutSettings() {
    if (!settingsWnd_) return;
    const Theme& t = theme_;
    int pad = t.s(24), w = t.s(480), inner = w - pad * 2, y = t.s(20);
    HDC dc = GetDC(settingsWnd_);
    size_t li = 0;  // next label in creation order
    auto nextLabel = [&] { return li < setLabels_.size() ? setLabels_[li++] : nullptr; };
    auto place = [&](HWND h, int x, int top, int cw, int ch) { MoveWindow(h, x, top, cw, ch, TRUE); };
    auto heading = [&] {
        place(nextLabel(), pad, y, inner, t.s(26));
        y += t.s(34);
    };

    heading();  // Playing
    place(setMinimize_, pad, y, inner, t.s(32));
    y += t.s(40);
    int lw = t.s(130);
    place(nextLabel(), pad, y + t.s(5), lw, t.s(22));  // Default player
    place(setPlayer_, pad + lw, y, inner - lw, t.s(240));
    y += t.s(44);
    place(setOffline_, pad, y, inner, t.s(32));
    y += t.s(36);
    place(setOriginals_, pad, y, inner, t.s(32));
    y += t.s(48);

    heading();  // Online
    place(setOnline_, pad, y, inner, t.s(32));
    y += t.s(36);
    HWND note = nextLabel();
    wchar_t buf[512];
    GetWindowTextW(note, buf, 512);
    int noteH = textHeight(dc, t.caption, buf, inner - t.s(28));
    place(note, pad + t.s(28), y, inner - t.s(28), noteH);  // lined up with the checkbox label
    y += noteH + t.s(10);
    place(setAutoUpdate_, pad, y, inner, t.s(32));
    y += t.s(42);
    place(nextLabel(), pad, y + t.s(6), lw, t.s(22));  // Your name
    int fieldH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE) + t.s(10);
    place(setName_, pad + lw, y, inner - lw, fieldH);
    y += fieldH + t.s(6);
    place(nextLabel(), pad + lw, y, inner - lw, t.s(20));  // what the name is for
    y += t.s(20) + t.s(18);

    heading();  // Folders
    place(nextLabel(), pad, y, inner, t.s(22));  // Game files
    y += t.s(24);
    place(nextLabel(), pad, y, inner, t.s(22));  // the folder path (setDirText_)
    y += t.s(30);
    int bh = t.s(34), bx = pad;
    for (int id : {kSetChange, kSetOpen, kSetReset}) {
        HWND b = GetDlgItem(settingsWnd_, id);
        wchar_t text[128];
        GetWindowTextW(b, text, 128);
        int bw = textWidth(dc, t.button, text) + t.s(32);
        place(b, bx, y, bw, bh);
        bx += bw + t.s(8);
    }
    ShowWindow(setReset_, user_.gamesDir.empty() ? SW_HIDE : SW_SHOW);
    y += bh + t.s(12);
    HWND logBtn = GetDlgItem(settingsWnd_, kSetLog);
    wchar_t logText[128];
    GetWindowTextW(logBtn, logText, 128);
    place(logBtn, pad, y, textWidth(dc, t.button, logText) + t.s(32), bh);
    y += bh + t.s(28);

    // Under a divider drawn in WM_PAINT: the version on its own line (beside the buttons it has no room and
    // wraps out of its box), then project page and Close.
    SetPropW(settingsWnd_, L"divider", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(y - t.s(14))));
    place(nextLabel(), pad, y, inner, t.s(22));
    y += t.s(32);
    int right = pad + inner;
    HWND close = GetDlgItem(settingsWnd_, kSetClose);
    place(close, right - t.s(100), y, t.s(100), bh);
    if (HWND web = GetDlgItem(settingsWnd_, kSetWeb)) place(web, right - t.s(100) - t.s(8) - t.s(130), y, t.s(130), bh);
    y += bh + t.s(20);
    ReleaseDC(settingsWnd_, dc);

    RECT rc{0, 0, w, y};
    AdjustWindowRectExForDpi(&rc, GetWindowLongW(settingsWnd_, GWL_STYLE), FALSE, GetWindowLongW(settingsWnd_, GWL_EXSTYLE),
                             t.dpi);
    RECT owner;
    GetWindowRect(hwnd_, &owner);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    int ow = owner.right - owner.left, oh = owner.bottom - owner.top;
    SetWindowPos(settingsWnd_, nullptr, owner.left + (ow - ww) / 2, owner.top + (oh - wh) / 3, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(settingsWnd_, nullptr, TRUE);
}

LRESULT CALLBACK Launcher::settingsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->settingsHandle(h, m, w, l);
}

LRESULT Launcher::settingsHandle(HWND h, UINT m, WPARAM w, LPARAM l) {
    const Theme& t = theme_;
    auto checked = [](HWND b) { return SendMessageW(b, BM_GETCHECK, 0, 0) == BST_CHECKED; };
    auto applyGamesDir = [&] {
        lib_.setGamesDirOverride(user_.gamesDir);
        refreshAll();
        checkOutdated();
        SetWindowTextW(setDirText_, lib_.vars().get(L"GAMES_DIR").c_str());
        save();
        layoutSettings();
    };
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
            int y = static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(h, L"divider")));
            RECT rc;
            GetClientRect(h, &rc);
            RECT line{t.s(24), y, rc.right - t.s(24), y + std::max(1, t.s(1))};
            HBRUSH br = CreateSolidBrush(t.c.border.ref());
            FillRect(dc, &line, br);
            DeleteObject(br);
            EndPaint(h, &ps);
            return 0;
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
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == kSetPlayer && code == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(setPlayer_, CB_GETCURSEL, 0, 0));
                user_.defaultPlayer = sel > 0 && sel <= static_cast<int>(cfg_.players.size()) ? cfg_.players[sel - 1].id
                                                                                              : std::string();
                save();
                refreshAll();
                return 0;
            }
            // The box announces its starting text while it is being created, before setName_ holds it: reading
            // it then gave an empty name that was saved over the real one. Only the finished box counts.
            if (id == kSetName && code == EN_CHANGE && setName_ && reinterpret_cast<HWND>(l) == setName_) {
                // "|" separates fields in the mods' messages; keep it out of names.
                wchar_t typed[64] = L"";
                GetWindowTextW(setName_, typed, 64);
                std::wstring name;
                for (const wchar_t* c = typed; *c; ++c)
                    if (*c != L'|') name += *c;
                user_.playerName = trim(name);
                save();
                return 0;
            }
            if (code != BN_CLICKED) break;
            switch (id) {
                case kSetAutoUpdate:
                    user_.autoUpdate = checked(setAutoUpdate_);
                    save();
                    return 0;
                case kSetOffline:
                    user_.showOffline = checked(setOffline_);
                    save();
                    refreshAll();
                    return 0;
                case kSetOriginals:
                    user_.showOriginals = checked(setOriginals_);
                    save();
                    refreshAll();
                    return 0;
                case kSetMinimize:
                    user_.minimize = checked(setMinimize_) ? 1 : 0;
                    save();
                    return 0;
                case kSetOnline:
                    user_.online = checked(setOnline_);
                    onlineOn_ = user_.online;
                    if (user_.online) {
                        if (roomsWake_) SetEvent(roomsWake_);  // refresh now, not in 30 seconds
                        if (!catalogThread_.joinable()) loadCatalog();
                    } else {
                        rooms_.clear();
                        roomsKnown_ = false;
                        InvalidateRect(grid_, nullptr, FALSE);
                        layoutDetail();
                    }
                    save();
                    return 0;
                case kSetChange: {
                    std::wstring dir = pickFolder(h, cfg_.t("settings.pickFolder"), lib_.vars().get(L"GAMES_DIR"));
                    if (!dir.empty()) {
                        user_.gamesDir = fs::normalize(dir);
                        applyGamesDir();
                    }
                    return 0;
                }
                case kSetOpen: {
                    std::wstring dir = lib_.vars().get(L"GAMES_DIR");
                    fs::ensureDir(dir, nullptr, nullptr);
                    sys::openPath(dir);
                    return 0;
                }
                case kSetReset:
                    user_.gamesDir.clear();
                    applyGamesDir();
                    SetFocus(GetDlgItem(h, kSetChange));  // the button that had focus is now hidden
                    return 0;
                case kSetLog:
                    sys::revealInExplorer(logger().path());
                    return 0;
                case kSetWeb:
                    sys::openPath(cfg_.branding.website);
                    return 0;
                case kSetClose:
                case IDCANCEL:  // Esc
                case IDOK:      // Enter
                    DestroyWindow(h);
                    return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            for (auto it = btnInfo_.begin(); it != btnInfo_.end();)
                it = GetParent(it->first) == h ? btnInfo_.erase(it) : std::next(it);
            if (hoverBtn_ && GetParent(hoverBtn_) == h) hoverBtn_ = nullptr;
            RemovePropW(h, L"divider");
            settingsWnd_ = nullptr;
            setLabels_.clear();
            EnableWindow(hwnd_, TRUE);
            SetForegroundWindow(hwnd_);
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace fl::ui
