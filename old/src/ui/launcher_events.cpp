// The launcher window: window procedures and message handling.
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
#include <xinput.h>  // types only: XInputGetState is looked up at run time, so no import library is linked

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

// ---- message handling -----------------------------------------------------

LRESULT CALLBACK Launcher::wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    Launcher* self = reinterpret_cast<Launcher*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<Launcher*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    }
    if (!self) return DefWindowProcW(h, m, w, l);
    return self->handle(m, w, l);
}

LRESULT CALLBACK Launcher::gridProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Launcher*>(ref);
    if (m == WM_MOUSEMOVE || m == WM_MOUSELEAVE) {
        int item = -1;
        if (m == WM_MOUSEMOVE) {
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            item = ListView_HitTest(h, &hit);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
        }
        if (item != self->hoverItem_) {
            RECT r;
            if (self->hoverItem_ >= 0 && ListView_GetItemRect(h, self->hoverItem_, &r, LVIR_BOUNDS))
                InvalidateRect(h, &r, FALSE);
            self->hoverItem_ = item;
            if (item >= 0 && ListView_GetItemRect(h, item, &r, LVIR_BOUNDS)) InvalidateRect(h, &r, FALSE);
            self->liftTick_ = GetTickCount64();
            self->wake();
            self->placeTilePlay();
        }
    } else if (m == WM_MOUSEWHEEL || m == WM_VSCROLL) {
        // The cards move under a still mouse; Play comes back with the next move.
        self->hoverItem_ = -1;
        self->placeTilePlay();
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        InvalidateRect(h, nullptr, FALSE);  // focus ring on the focused card
    } else if (m == WM_SIZE) {
        LRESULT r = DefSubclassProc(h, m, w, l);
        self->layoutGrid();
        return r;
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT CALLBACK Launcher::searchProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    // The hint is drawn here instead of with EM_SETCUEBANNER: Windows draws that in
    // the system gray, which is only 3.1:1 on the dark search box.
    LRESULT r = DefSubclassProc(h, m, w, l);
    // WM_PRINT too: the opening entrance draws the box into its picture.
    bool print = m == WM_PRINT || m == WM_PRINTCLIENT;
    if ((m == WM_PAINT || print) && GetWindowTextLengthW(h) == 0) {
        auto* self = reinterpret_cast<Launcher*>(ref);
        HDC dc = print ? reinterpret_cast<HDC>(w) : GetDC(h);
        RECT rc;
        GetClientRect(h, &rc);
        rc.left += self->theme_.s(4);
        HGDIOBJ old = SelectObject(dc, self->theme_.body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, self->theme_.c.textMuted.ref());
        std::wstring cue = self->cfg_.t(h == self->search_ ? "search.cue" : "roomsPage.code");
        DrawTextW(dc, cue.c_str(), static_cast<int>(cue.size()), &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(dc, old);
        if (!print) ReleaseDC(h, dc);
    }
    return r;
}

LRESULT CALLBACK Launcher::buttonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<Launcher*>(ref);
    if (m == WM_MOUSEMOVE && self->hoverBtn_ != h) {
        HWND old = self->hoverBtn_;
        self->hoverBtn_ = h;
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        if (old) InvalidateRect(old, nullptr, FALSE);
        InvalidateRect(h, nullptr, FALSE);
    } else if (m == WM_MOUSELEAVE && self->hoverBtn_ == h) {
        self->hoverBtn_ = nullptr;
        InvalidateRect(h, nullptr, FALSE);
        if (h == self->tilePlayBtn_) self->placeTilePlay();  // off the card's Play and off the card: it goes
    } else if (m == WM_ERASEBKGND) {
        return 1;  // drawn completely in NM_CUSTOMDRAW
    }
    return DefSubclassProc(h, m, w, l);
}

LRESULT Launcher::handle(UINT m, WPARAM w, LPARAM l) {
    if (intro_ && (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MOUSEWHEEL || m == WM_KEYDOWN ||
                   m == WM_SYSKEYDOWN))
        endIntro();
    switch (m) {
        case WM_SIZE:
            if (w != SIZE_MINIMIZED) relayout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(l);
            UINT dpi = GetDpiForWindow(hwnd_);
            mm->ptMinTrackSize = {MulDiv(cfg_.branding.window.minWidth, dpi, 96),
                                  MulDiv(cfg_.branding.window.minHeight, dpi, 96)};
            return 0;
        }
        case WM_DPICHANGED: {
            theme_.build(HIWORD(w));
            applyFonts();
            artCache_.clear();
            tileW_ = tileH_ = 0;
            auto* r = reinterpret_cast<RECT*>(l);
            SetWindowPos(hwnd_, nullptr, r->left, r->top, width(*r), height(*r), SWP_NOZORDER | SWP_NOACTIVATE);
            relayout();
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_SYSCOLORCHANGE:
            motion_ = !snapshot_ && systemAnimations();
            if (!motion_) {  // turned off mid-way: jump to the end
                titleFrom_ = 0;
                for (auto& [code, from] : codeFrom_) from = 0;
                joinFrom_.clear();
            }
            theme_.build(theme_.dpi);
            applyFonts();
            artCache_.clear();
            relayout();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd_, &ps);
            paint(dc, ps.rcPaint);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(w), theme_.c.text.ref());
            SetBkColor(reinterpret_cast<HDC>(w), theme_.c.surface.ref());
            return reinterpret_cast<LRESULT>(theme_.brSurface);
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(w);
            bool sidebar = reinterpret_cast<HWND>(l) == nav_;
            SetTextColor(dc, (sidebar ? theme_.c.sidebarText : theme_.c.text).ref());
            SetBkColor(dc, (sidebar ? theme_.c.sidebar : theme_.c.surface).ref());
            return reinterpret_cast<LRESULT>(sidebar ? theme_.brSidebar : theme_.brSurface);
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(l);
            if (mi->CtlID == kIdNav) mi->itemHeight = theme_.s(38);
            return TRUE;
        }
        case WM_DRAWITEM: {
            auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(l);
            if (d->CtlID == kIdNav) drawNavItem(*d);
            return TRUE;
        }
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
            if (hdr->hwndFrom != grid_) break;
            if (hdr->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(l);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int idx = static_cast<int>(cd->nmcd.dwItemSpec);
                    RECT r;
                    if (idx >= 0 && idx < static_cast<int>(visible_.size()) &&
                        ListView_GetItemRect(grid_, idx, &r, LVIR_BOUNDS)) {
                        UINT state = ListView_GetItemState(grid_, idx, LVIS_SELECTED | LVIS_FOCUSED);
                        drawCard(cd->nmcd.hdc, visible_[idx], r, (state & LVIS_SELECTED) != 0,
                                 (state & LVIS_FOCUSED) && GetFocus() == grid_);
                    }
                    return CDRF_SKIPDEFAULT;
                }
                return CDRF_DODEFAULT;
            }
            if (hdr->code == LVN_ITEMCHANGED && !suppressSelect_) {
                auto* nm = reinterpret_cast<NMLISTVIEW*>(l);
                if ((nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED) && !(nm->uOldState & LVIS_SELECTED))
                    select(static_cast<int>(nm->lParam));
            } else if (hdr->code == NM_CLICK || hdr->code == LVN_ITEMACTIVATE) {
                // A click on a card opens its page: Play, Host and the rest live there.
                auto* nm = reinterpret_cast<NMITEMACTIVATE*>(l);
                if (nm->iItem >= 0 && nm->iItem < static_cast<int>(visible_.size())) {
                    select(visible_[nm->iItem]);
                    openGuide(visible_[nm->iItem]);
                }
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(w), code = HIWORD(w);
            if (id == kIdNav && code == LBN_SELCHANGE) {
                navIndex_ = static_cast<size_t>(SendMessageW(nav_, LB_GETCURSEL, 0, 0));
                SendMessageW(navCombo_, CB_SETCURSEL, navIndex_, 0);
                user_.category = navItems_[navIndex_].key;
                closeGuide();
                rebuildGrid();
            } else if (id == kIdNavCombo && code == CBN_SELCHANGE) {
                navIndex_ = static_cast<size_t>(SendMessageW(navCombo_, CB_GETCURSEL, 0, 0));
                SendMessageW(nav_, LB_SETCURSEL, navIndex_, 0);
                user_.category = navItems_[navIndex_].key;
                closeGuide();
                rebuildGrid();
            } else if (id == kIdSort && code == CBN_SELCHANGE) {
                user_.sortMode = std::clamp(static_cast<int>(SendMessageW(sortCombo_, CB_GETCURSEL, 0, 0)), 0, 3);
                save();
                rebuildGrid();
            } else if (id == kIdSearch && code == EN_CHANGE) {
                closeGuide();
                rebuildGrid();
            } else if (id == kIdSearch && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                InvalidateRect(hwnd_, &searchBox_, FALSE);
            } else if (id == kIdPlayer && code == CBN_SELCHANGE) {
                onPlayerChoice();
            } else if (id == kIdSave && code == CBN_SELCHANGE) {
                onSaveChoice();
            } else if (id >= kIdSettingCombo0 && id <= kIdSettingCombo3 && code == CBN_SELCHANGE) {
                onSettingChanged(id - kIdSettingCombo0, true);
            } else if (id == kIdPageGame && code == CBN_SELCHANGE) {
                pageScroll_ = 0;
                layoutRoomsPage();
                InvalidateRect(hwnd_, &gridRc_, FALSE);
            } else if ((id == kIdTabCode || id == kIdPageCode) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
                InvalidateRect(hwnd_, id == kIdTabCode ? &dl_.codeBox : &pageCodeBox_, FALSE);
            } else if (code == BN_CLICKED) {
                if (id == kIdHostGame) onHostGame();
                else if (id == kIdJoinGame) onJoinGame();
                else if (id >= kIdSetting0 && id <= kIdSetting3) onSettingChanged(id - kIdSetting0, false);
                else if (id >= kIdAction0 && id < kIdRoom0) runAction(selected_, static_cast<size_t>(id - kIdAction0));
                else if (id == kIdFav) toggleFavorite();
                else if (id == kIdLocate) locateFile();
                else if (id == kIdFolder) openFolder();
                else if (id == kIdTabLibrary) {
                    if (roomsPage()) openNav(libraryKey_.empty() || libraryKey_ == "rooms" ? navItems_.front().key : libraryKey_);
                } else if (id == kIdTabRoomsTop) {
                    if (!roomsPage()) openNav("rooms");
                } else if (id == kIdHeroPlay) {
                    if (heroGame_ >= 0 && heroAction_ >= 0) {
                        select(heroGame_);
                        runAction(heroGame_, static_cast<size_t>(heroAction_));
                    }
                } else if (id == kIdTilePlay) {
                    if (tilePlayGame_ >= 0 && tilePlayAction_ >= 0) {
                        int gi = tilePlayGame_, ai = tilePlayAction_;
                        select(gi);
                        runAction(gi, static_cast<size_t>(ai));
                    }
                } else if (id >= kIdPageTab0 && id <= kIdPageTab2) {
                    showPageTab(id - kIdPageTab0);
                } else if (id == kIdCatChip) {
                    dropList(catBtn_, navCombo_, kIdNavCombo);
                } else if (id == kIdSortChip) {
                    dropList(sortBtn_, sortCombo_, kIdSort);
                } else if (id == kIdSize) {
                    user_.compactCards = !user_.compactCards;
                    save();
                    SetWindowTextW(sizeBtn_, cfg_.t(user_.compactCards ? "button.normalCards" : "button.compactCards").c_str());
                    relayout();
                } else if (id == kIdHero) {
                    if (heroGame_ >= 0) {
                        select(heroGame_);
                        auto at = std::find(visible_.begin(), visible_.end(), heroGame_);
                        if (at != visible_.end()) ListView_EnsureVisible(grid_, static_cast<int>(at - visible_.begin()), FALSE);
                        openGuide(heroGame_);
                    }
                } else if (id == kIdAbout) {
                    if (guidePage() && guideGame_ == selected_) closeGuide();
                    else openGuide(selected_);
                } else if (id == kIdGuideBack) closeGuide();
                else if (id >= kIdGuideLink0 && id <= kIdGuideLink3) {
                    size_t i = static_cast<size_t>(id - kIdGuideLink0);
                    if (i < guide_.links.size()) sys::openPath(guide_.links[i].url);
                }
                else if (id == kIdGetPlayer) getPlayer();
                else if (id == kIdStopHost) {
                    if (Session* s = webHostSession(selected_)) {
                        auto it = std::find_if(sessions_.begin(), sessions_.end(),
                                               [&](const Session& x) { return x.id == s->id; });
                        endSession(it);
                        notice(cfg_.t("notice.relayStopped"), theme_.c.textMuted);
                        select(selected_);
                    } else {
                        stopRelay(true);
                    }
                }
                else if (id == kIdCopy) copyAddress();
                else if (id == kIdDownload && selected_ >= 0) offerGameDownload(cfg_.games[selected_]);
                else if (id == kIdSettings) openSettings();
                else if (id >= kIdRoom0 && id < kIdPageRoom0) joinRoom(static_cast<size_t>(id - kIdRoom0));
                else if (id >= kIdPageRoom0 && id < kIdPageRoomEnd) {
                    size_t i = static_cast<size_t>(id - kIdPageRoom0);
                    if (i < pageRows_.size()) joinListedRoom(pageRows_[i].game, pageRows_[i].room);
                } else if (id == kIdTabPlay || id == kIdTabRooms) {
                    detailTab_ = id == kIdTabRooms ? 1 : 0;
                    layoutDetail();
                } else if (id == kIdMore) {
                    showMore_ = !showMore_;
                    layoutDetail();
                } else if (id == kIdQuick) quickPlay();
                else if (id == kIdTabCodeJoin) joinByCode(tabCodeEdit_, selected_);
                else if (id == kIdPageCodeJoin) joinByCode(pageCodeEdit_, -1);
                else if (id == kIdPageHideFull) {
                    pageScroll_ = 0;
                    layoutRoomsPage();
                    InvalidateRect(hwnd_, &gridRc_, FALSE);
                }
                else if (id == IDOK && selected_ >= 0) runAction(selected_, 0);  // Enter outside the grid
            }
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (guidePage()) {
                scrollGuide(-GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * theme_.s(48));
                return 0;
            }
            if (roomsPage()) {
                pageScroll_ -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
                layoutRoomsPage();
                InvalidateRect(hwnd_, &gridRc_, FALSE);
                return 0;
            }
            break;
        case WM_TIMER:
            if (w == kFrameTimer) {
                onFrame();
                return 0;
            }
            if (w == kPadTimer) {
                pollPad(false);
                return 0;
            }
            if (!pad_) pollPad(true);  // once a second: has a controller been plugged in?
            if (toastFrom_ && !motion_ && since(toastFrom_) > kToastMs) {  // no frames run without animations
                toastFrom_ = 0;
                ShowWindow(toast_, SW_HIDE);
            }
            if (++modPollTick_ % 2 == 0) pollModGames();
            if (!sessions_.empty() || (noticeUntil_ && GetTickCount64() >= noticeUntil_)) {
                if (GetTickCount64() >= noticeUntil_) noticeUntil_ = 0;
                InvalidateRect(hwnd_, &status_, FALSE);
            }
            return 0;
        case WM_APP_EXIT:
            onProcessExit(static_cast<DWORD>(w), static_cast<DWORD>(l));
            return 0;
        case WM_APP_INSTALL:
            InvalidateRect(hwnd_, &status_, FALSE);
            return 0;
        case WM_APP_INSTALLED:
            onInstalled();
            return 0;
        case WM_APP_ROOMS:
            onRooms();
            fillLobbyRooms();
            if (lobby_ && lobbySnap_.host && pingMs_ >= 0) lobby_->setServerPing(pingMs_);
            return 0;
        case WM_APP_SITE:
            onSite(static_cast<int>(w));
            return 0;
        case WM_APP_LOBBY:
            onLobby();
            return 0;
        case WM_APP_LOBBYNET: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(l));
            if (lobby_) {
                lobbyNetText_ = *text;
                updateLobbyRoom();
            }
            return 0;
        }
        case WM_APP_MODS:
            onModsFetched();
            return 0;
        case WM_APP_MODPROGRESS:
            InvalidateRect(hwnd_, &status_, FALSE);
            for (auto& [b, info] : btnInfo_)
                if (info.progress && IsWindowVisible(b)) InvalidateRect(b, nullptr, FALSE);
            return 0;
        case WM_APP_MODDONE:
            onModDone(reinterpret_cast<ModDone*>(l));
            return 0;
        case WM_APP_SERVERLINE: {
            std::unique_ptr<ServerLine> line(reinterpret_cast<ServerLine*>(l));
            onServerLine(line->session, line->text);
            return 0;
        }
        case WM_APP_UPDATED: {
            std::unique_ptr<std::wstring> version(reinterpret_cast<std::wstring*>(l));
            onUpdated(*version);
            return 0;
        }
        case WM_APP_HANDOVER: {
            std::unique_ptr<std::wstring> installer(reinterpret_cast<std::wstring*>(l));
            onHandover(*installer);
            return 0;
        }
        case WM_APP_RATELIMIT: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(l));
            toast(*text, theme_.c.error);
            return 0;
        }
        case WM_APP_PLAYERUPDATED: {
            std::unique_ptr<std::wstring> what(reinterpret_cast<std::wstring*>(l));
            onPlayerUpdated(*what);
            return 0;
        }
        case WM_APP_SERVERREADY:
            onServerReady(static_cast<int>(w), l != 0);
            return 0;
        case WM_APP_CATALOG: {
            std::string raw;
            {
                std::lock_guard<std::mutex> lock(installMu_);
                raw.swap(catalogRaw_);
                if (pendingOutdatedReady_) {
                    outdated_ = std::move(pendingOutdated_);
                    pendingOutdated_.clear();
                    pendingOutdatedReady_ = false;
                }
            }
            std::vector<CatalogFile> files;
            std::wstring err;
            if (raw.empty()) {
                InvalidateRect(grid_, nullptr, FALSE);
                select(selected_);
            } else if (parseCatalog(raw, cfg_.downloads.catalog, files, err)) {
                catalog_ = std::move(files);
                parseCatalogSettings(raw, catalogSettings_);
                std::wstring cache = fs::join(lib_.dataDir(), L"catalog.json");
                fs::ensureDir(lib_.dataDir(), nullptr, nullptr);
                fs::writeFile(cache, raw.data(), raw.size(), nullptr);
                select(selected_);
            }
            return 0;
        }
        case WM_APP_FIRSTRUN:
            for (const auto& p : cfg_.players)
                if (p.install.enabled()) {
                    offerInstall(p, true);
                    break;
                }
            return 0;
        case WM_APP_RELAY: {
            size_t peers = relay_.running() ? relay_.snapshot().peers.size() : 0;
            if (motion_)
                for (size_t i = slotPeers_; i < peers; ++i) joinFrom_[static_cast<int>(i)] = GetTickCount64();
            slotPeers_ = peers;
            layoutDetail();
            return 0;
        }
        case WM_CLOSE: {
            WINDOWPLACEMENT wp{sizeof wp};
            if (GetWindowPlacement(hwnd_, &wp)) {
                user_.x = wp.rcNormalPosition.left;
                user_.y = wp.rcNormalPosition.top;
                user_.width = width(wp.rcNormalPosition);
                user_.height = height(wp.rcNormalPosition);
                user_.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
            }
            save();
            DestroyWindow(hwnd_);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kTimer);
            hwnd_ = nullptr;
            if (!snapshot_) PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

// ---- game controller -------------------------------------------------------

void Launcher::pollPad(bool connectOnly) {
    if (!padTried_) {
        padTried_ = true;
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput9_1_0.dll"}) {
            if (HMODULE m = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
                padGetState_ = reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState"));
            if (padGetState_) break;
        }
    }
    if (!padGetState_ || snapshot_) return;
    using GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    auto getState = reinterpret_cast<GetState>(padGetState_);
    XINPUT_STATE s{};
    bool connected = false;
    for (DWORD i = 0; i < XUSER_MAX_COUNT && !connected; ++i) connected = getState(i, &s) == ERROR_SUCCESS;
    unsigned b = connected ? s.Gamepad.wButtons : 0;
    // The left stick past a third of its travel counts as the D-pad.
    const SHORT dead = 12000;
    if (s.Gamepad.sThumbLY > dead) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (s.Gamepad.sThumbLY < -dead) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (s.Gamepad.sThumbLX < -dead) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (s.Gamepad.sThumbLX > dead) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (connected != pad_) {
        pad_ = connected;
        padButtons_ = b;  // what is held as it connects is not a press
        padRepeatKey_ = 0;
        if (connected) SetTimer(hwnd_, kPadTimer, 50, nullptr);
        else KillTimer(hwnd_, kPadTimer);
        InvalidateRect(hwnd_, &status_, FALSE);  // the hints switch between keys and buttons
        return;
    }
    if (!connected || connectOnly) return;
    unsigned pressed = b & ~padButtons_;
    padButtons_ = b;
    // Only into this launcher's own windows (Settings and the lobby included), and only while one is in front.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) {
        padRepeatKey_ = 0;
        return;
    }
    auto key = [](WORD vk, bool shift = false) {
        INPUT in[4]{};
        UINT n = 0;
        auto add = [&](WORD k, bool up) {
            in[n].type = INPUT_KEYBOARD;
            in[n].ki.wVk = k;
            in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            ++n;
        };
        if (shift) add(VK_SHIFT, false);
        add(vk, false);
        add(vk, true);
        if (shift) add(VK_SHIFT, true);
        SendInput(n, in, sizeof(INPUT));
    };
    struct Dir {
        unsigned bit;
        WORD vk;
    };
    const Dir dirs[] = {{XINPUT_GAMEPAD_DPAD_UP, VK_UP}, {XINPUT_GAMEPAD_DPAD_DOWN, VK_DOWN},
                        {XINPUT_GAMEPAD_DPAD_LEFT, VK_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, VK_RIGHT}};
    ULONGLONG now = GetTickCount64();
    // A held direction repeats, like a held arrow key: after a pause, then steadily.
    for (const Dir& d : dirs) {
        if (pressed & d.bit) {
            key(d.vk);
            padRepeatKey_ = d.bit;
            padRepeatAt_ = now + 400;
        }
    }
    if (padRepeatKey_ && !(b & padRepeatKey_)) padRepeatKey_ = 0;
    if (padRepeatKey_ && now >= padRepeatAt_) {
        for (const Dir& d : dirs)
            if (d.bit == padRepeatKey_) key(d.vk);
        padRepeatAt_ = now + 120;
    }
    if (pressed & XINPUT_GAMEPAD_A) {
        // Space presses a focused button; Enter opens a card (and answers everything else).
        wchar_t cls[16] = {};
        GetClassNameW(GetFocus(), cls, 16);
        key(_wcsicmp(cls, L"Button") == 0 ? VK_SPACE : VK_RETURN);
    }
    if (pressed & XINPUT_GAMEPAD_B) key(VK_ESCAPE);
    if (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) key(VK_TAB);
    if (pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) key(VK_TAB, true);
    if ((pressed & XINPUT_GAMEPAD_Y) && GetForegroundWindow() == hwnd_ && !guidePage()) {
        SetFocus(search_);
        SendMessageW(search_, EM_SETSEL, 0, -1);
    }
}

}  // namespace fl::ui
