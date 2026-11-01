// The launcher window: the few animations and the frame timer.
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

// ---- animation -------------------------------------------------------------

bool Launcher::pulsing() const {
    if (!motion_ || selected_ < 0 || !relay_.running() || relayGame_ != selected_ || IsIconic(hwnd_)) return false;
    return static_cast<int>(slotPeers_) < cfg_.games[selected_].maxPlayers;
}

void Launcher::wake() {
    if (!motion_ || frameTimer_ || !hwnd_) return;
    frameTimer_ = SetTimer(hwnd_, kFrameTimer, kFrameMs, nullptr) != 0;
}

void Launcher::onFrame() {
    if (intro_) {
        if (since(intro_->from) < (intro_->page ? kPageMs : kIntroMs)) {
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        endIntro();
    }
    bool any = false;
    auto redraw = [&](const RECT& r) {
        InvalidateRect(hwnd_, &r, FALSE);
        any = true;
    };
    // A finished animation still gets this frame, painted with its final look
    // (since() of 0 is "long ago"). Timer messages can arrive late, so the end
    // is marked here rather than guessed from the clock.
    auto step = [](ULONGLONG& from, ULONGLONG duration) {
        if (!from) return false;
        if (since(from) >= duration) from = 0;
        return true;
    };
    if (step(titleFrom_, kTitleMs)) redraw(dl_.title);
    bool decoding = false;
    for (auto& [code, from] : codeFrom_) decoding = step(from, kCodeMs) || decoding;
    if (decoding) {
        redraw(dl_.rooms);
        if (roomsPage()) redraw(pageList_);
    }
    if ((!lift_.empty() || hoverItem_ >= 0) && stepLift()) any = true;
    if (anyBusy()) {
        any = true;
        for (auto& [b, info] : btnInfo_)
            if (info.busy && IsWindowVisible(b)) InvalidateRect(b, nullptr, FALSE);
    }
    if (toastFrom_) {
        any = true;
        layoutToast();
    }
    bool joining = false;
    for (auto& [slot, from] : joinFrom_) joining = step(from, kJoinMs) || joining;
    if (joining || pulsing()) {
        // A joining slot grows a little past its rectangle.
        RECT r = dl_.slots;
        InflateRect(&r, theme_.s(6), theme_.s(4));
        redraw(r);
    }
    if (!any || !motion_) {
        KillTimer(hwnd_, kFrameTimer);
        frameTimer_ = false;
    }
}

void Launcher::startIntro(int page) {
    if (!motion_ || intro_) return;
    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;
    // The title decodes once the panel has arrived, not inside the picture.
    titleFrom_ = 0;
    HWND focus = GetFocus();
    HDC screen = GetDC(hwnd_);
    Buffer buf(screen, cr.right, cr.bottom);
    ReleaseDC(hwnd_, screen);
    paint(buf.dc, cr);
    auto intro = std::make_unique<Intro>();
    std::vector<HWND> children;
    for (HWND h = GetWindow(hwnd_, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT))
        if (IsWindowVisible(h)) children.push_back(h);
    // Bottom of the z-order first, so overlapping controls end up as on screen.
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        RECT r;
        GetWindowRect(*it, &r);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&r), 2);
        POINT old;
        SetViewportOrgEx(buf.dc, r.left, r.top, &old);
        SendMessageW(*it, WM_PRINT, reinterpret_cast<WPARAM>(buf.dc),
                     PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        SetViewportOrgEx(buf.dc, old.x, old.y, nullptr);
    }
    intro->hidden = children;

    RECT gw, gcl;
    GetWindowRect(grid_, &gw);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&gw), 2);
    GetClientRect(grid_, &gcl);
    if (IsWindowVisible(grid_)) {
        int n = ListView_GetItemCount(grid_);
        for (int i = 0; i < n; ++i) {
            RECT ir;
            if (!ListView_GetItemRect(grid_, i, &ir, LVIR_BOUNDS) || ir.bottom <= 0 || ir.top >= gcl.bottom) continue;
            OffsetRect(&ir, gw.left, gw.top);
            intro->cards.push_back(ir);
        }
        intro->gridBar = {gw.left + gcl.right, gw.top, gw.right, gw.bottom};
    }

    SelectObject(buf.dc, buf.old);  // GDI+ must not read a bitmap still selected into a DC
    {
        Bitmap src(buf.bmp, nullptr);
        intro->shot = std::make_unique<Bitmap>(cr.right, cr.bottom, PixelFormat32bppPARGB);
        Graphics g(intro->shot.get());
        g.DrawImage(&src, 0, 0, cr.right, cr.bottom);
    }
    buf.old = SelectObject(buf.dc, buf.bmp);

    for (HWND h : children) ShowWindow(h, SW_HIDE);
    intro->page = page;
    intro->focus = focus;
    intro->from = GetTickCount64();
    intro_ = std::move(intro);
    InvalidateRect(hwnd_, nullptr, FALSE);
    wake();
}

void Launcher::endIntro() {
    if (!intro_) return;
    std::unique_ptr<Intro> intro = std::move(intro_);
    for (HWND h : intro->hidden) ShowWindow(h, SW_SHOWNA);
    if (intro->focus && IsWindowVisible(intro->focus)) SetFocus(intro->focus);
    else if (!GetFocus() || GetFocus() == hwnd_) SetFocus(grid_);
    if (intro->layoutLater) layoutDetail();
    if (motion_ && selected_ >= 0) titleFrom_ = GetTickCount64();
    InvalidateRect(hwnd_, nullptr, FALSE);
    wake();
}

void Launcher::paintIntro(Graphics& g) {
    const Theme& t = theme_;
    Bitmap& shot = *intro_->shot;
    ULONGLONG e = since(intro_->from);
    // 0..1 between `start` and `start + duration` ms, eased out.
    auto at = [e](ULONGLONG start, ULONGLONG duration) {
        return motion::easeOutCubic(e <= start ? 0.0f : static_cast<float>(e - start) / duration);
    };
    // A piece of the finished frame, moved by (dx, dy) and faded to `alpha`.
    auto piece = [&](const RECT& src, float dx, float dy, float alpha) {
        if (alpha <= 0 || width(src) <= 0 || height(src) <= 0) return;
        RectF dst(src.left + dx, src.top + dy, static_cast<REAL>(width(src)), static_cast<REAL>(height(src)));
        ColorMatrix m = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, alpha, 0}, {0, 0, 0, 0, 1}}};
        ImageAttributes fade;
        fade.SetColorMatrix(&m);
        g.DrawImage(&shot, dst, static_cast<REAL>(src.left), static_cast<REAL>(src.top), dst.Width, dst.Height,
                    UnitPixel, alpha < 1 ? &fade : nullptr);
    };
    SolidBrush bg(gc(t.c.background));
    g.FillRectangle(&bg, 0, 0, static_cast<INT>(shot.GetWidth()), static_cast<INT>(shot.GetHeight()));

    if (intro_->page) {
        // A page change: the top bar and the status bar stay; what is between them slides in a little from the
        // side it comes from (a game's page from the right, the library back from the left) as it fades in.
        RECT body{0, topBar_.bottom, static_cast<LONG>(shot.GetWidth()), status_.top};
        float p = motion::easeOutCubic(std::min(1.0f, static_cast<float>(e) / kPageMs));
        piece(topBar_, 0, 0, 1);
        piece(status_, 0, 0, 1);
        piece(body, t.sf(28) * intro_->page * (1 - p), 0, p);
        return;
    }

    // The details panel slides in from the right edge.
    float d = at(90, 480);
    piece(detail_, width(detail_) * 0.35f * (1 - d), 0, d);
    // Cards rise into place one after another, like a deal.
    Region old;
    g.GetClip(&old);
    g.SetClip(rf(gridRc_), CombineModeIntersect);
    for (size_t i = 0; i < intro_->cards.size(); ++i) {
        float c = at(160 + std::min<ULONGLONG>(i, 7) * 45, 340);
        piece(intro_->cards[i], 0, t.sf(14) * (1 - c), c);
    }
    piece(intro_->gridBar, 0, 0, at(400, 300));
    g.SetClip(&old);
    // The sidebar comes in from the left, the search bar drops in.
    float sb = at(0, 460);
    piece(sidebar_, -width(sidebar_) * 0.5f * (1 - sb), 0, sb);
    float tp = at(120, 380);
    piece(top_, 0, -t.sf(8) * (1 - tp), tp);
    piece(status_, 0, 0, at(350, 350));
}

}  // namespace fl::ui
