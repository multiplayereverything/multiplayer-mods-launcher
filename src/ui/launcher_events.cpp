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
        }
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
            } else if (hdr->code == LVN_ITEMACTIVATE) {
                auto* nm = reinterpret_cast<NMITEMACTIVATE*>(l);
                if (nm->iItem >= 0 && nm->iItem < static_cast<int>(visible_.size())) runAction(visible_[nm->iItem], 0);
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

}  // namespace fl::ui
