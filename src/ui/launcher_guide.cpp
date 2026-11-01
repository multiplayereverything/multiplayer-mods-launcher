// The guide page: a game's steps, keys and tips (core/guide.h) shown in place of the game grid. Painted
// text with a mouse-wheel scroll, plus real buttons for Back and the links.
#include <algorithm>

#include "core/guide.h"
#include "core/process.h"
#include "core/strings.h"
#include "launcher.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

namespace {

using Kind = GuideBlock::Kind;

constexpr int kLabelW = 150;  // 96-DPI units: the label column of a fact
constexpr int kKeyW = 96;     // the key column of a key

}  // namespace

bool Launcher::guidePage() const { return guideGame_ >= 0 && guideGame_ < static_cast<int>(cfg_.games.size()); }

void Launcher::openGuide(int gi) {
    if (gi < 0 || gi >= static_cast<int>(cfg_.games.size())) return;
    guideGame_ = gi;
    guide_ = buildGuide(cfg_, cfg_.games[gi], siteOf(gi));
    guideScroll_ = 0;
    ShowWindow(grid_, SW_HIDE);
    layoutHero();
    layoutGuidePage();
    InvalidateRect(hwnd_, &gridRc_, FALSE);
    if (guideBack_) SetFocus(guideBack_);
}

void Launcher::closeGuide() {
    if (!guidePage()) return;
    guideGame_ = -1;
    guide_ = {};
    guideRows_.clear();
    layoutHero();
    layoutGrid();
    layoutGuidePage();
    ShowWindow(grid_, visible_.empty() || roomsPage() ? SW_HIDE : SW_SHOW);
    InvalidateRect(hwnd_, &gridRc_, FALSE);
}

// Measures every block for the page's width; the rows keep where each one starts, so painting is a loop.
void Launcher::layoutGuidePage() {
    if (!guideBack_) return;
    std::vector<HWND> controls = {guideBack_};
    for (HWND h : guideLinkBtns_) controls.push_back(h);
    if (!guidePage() || width(gridRc_) <= 0) {
        for (HWND h : controls) ShowWindow(h, SW_HIDE);
        guideRows_.clear();
        guideView_ = guideHead_ = guideFoot_ = {};
        return;
    }
    const Theme& t = theme_;
    HDC dc = GetDC(hwnd_);
    int pad = t.s(kPad), x = gridRc_.left + pad, w = width(gridRc_) - pad * 2;
    int y = gridRc_.top + t.s(4);

    // Header: Back, and the page's title.
    int backW = textWidth(dc, t.button, cfg_.t("guide.back")) + t.s(32), backH = t.s(34);
    SetWindowTextW(guideBack_, cfg_.t("guide.back").c_str());
    MoveWindow(guideBack_, x, y, backW, backH, TRUE);
    std::wstring title = cfg_.t("guide.title", {{L"NAME", displayName(cfg_.games[guideGame_])}});
    int titleH = textHeight(dc, t.heading, title, w - backW - t.s(16), DT_SINGLELINE);
    guideHead_ = {x + backW + t.s(16), y + (backH - titleH) / 2, x + w, y + (backH - titleH) / 2 + titleH};
    y += backH + t.s(14);

    // Footer: one button per link.
    int footH = guide_.links.empty() ? 0 : t.s(48);
    guideFoot_ = {x, gridRc_.bottom - pad - footH, x + w, gridRc_.bottom - pad};
    std::set<HWND> shown = {guideBack_};
    int bx = x;
    for (size_t i = 0; i < guide_.links.size() && i < guideLinkBtns_.size(); ++i) {
        std::wstring label = guide_.links[i].label;
        int bw = textWidth(dc, t.button, label) + t.s(32);
        if (bx + bw > x + w) break;
        SetWindowTextW(guideLinkBtns_[i], label.c_str());
        MoveWindow(guideLinkBtns_[i], bx, guideFoot_.top + t.s(6), bw, footH - t.s(6), TRUE);
        shown.insert(guideLinkBtns_[i]);
        bx += bw + t.s(8);
    }
    guideView_ = {x, y, x + w, guideFoot_.top - (footH ? t.s(8) : 0)};

    // Rows.
    guideRows_.clear();
    int cy = 0, textW = w - t.s(4);
    for (size_t i = 0; i < guide_.blocks.size(); ++i) {
        const GuideBlock& b = guide_.blocks[i];
        GuideRow r;
        r.block = i;
        int h = 0;
        switch (b.kind) {
            case Kind::Heading:
                cy += i == 0 ? 0 : t.s(18);
                h = textHeight(dc, t.heading, b.text, textW, DT_SINGLELINE) + t.s(6);
                break;
            case Kind::Text:
                h = textHeight(dc, t.body, b.text, textW) + t.s(8);
                break;
            case Kind::Step:
                h = std::max(t.s(26), textHeight(dc, t.body, b.text, textW - t.s(40))) + t.s(8);
                break;
            case Kind::Bullet:
                h = textHeight(dc, t.body, b.text, textW - t.s(22)) + t.s(6);
                break;
            case Kind::Key: {
                int detailW = textW - t.s(kKeyW) - t.s(12);
                h = std::max(t.s(28), textHeight(dc, t.body, b.detail, detailW)) + t.s(6);
                break;
            }
            case Kind::Fact: {
                int valueW = textW - t.s(kLabelW) - t.s(12);
                h = std::max(textHeight(dc, t.caption, b.text, t.s(kLabelW)), textHeight(dc, t.body, b.detail, valueW)) + t.s(6);
                break;
            }
        }
        r.y = cy;
        r.h = h;
        guideRows_.push_back(r);
        cy += h;
    }
    guideContentH_ = cy;
    int viewH = height(guideView_);
    guideScroll_ = std::clamp(guideScroll_, 0, std::max(0, guideContentH_ - viewH));
    ReleaseDC(hwnd_, dc);
    for (HWND h : controls) ShowWindow(h, shown.count(h) ? SW_SHOW : SW_HIDE);
    for (HWND h : controls) InvalidateRect(h, nullptr, FALSE);
}

void Launcher::scrollGuide(int pixels) {
    if (!guidePage()) return;
    int maxScroll = std::max(0, guideContentH_ - height(guideView_));
    int next = std::clamp(guideScroll_ + pixels, 0, maxScroll);
    if (next == guideScroll_) return;
    guideScroll_ = next;
    InvalidateRect(hwnd_, &gridRc_, FALSE);
}

void Launcher::paintGuidePage(Graphics& g, HDC dc) {
    if (!guidePage() || height(guideView_) <= 0) return;
    const Theme& t = theme_;
    (void)dc;
    drawText(g, cfg_.t("guide.title", {{L"NAME", displayName(cfg_.games[guideGame_])}}), guideHead_, t.heading, t.c.text,
             DT_SINGLELINE | DT_END_ELLIPSIS);
    // A line under the header and over the link bar, so the scrolling part reads as its own area.
    Pen line(gc(t.c.border), 1);
    g.DrawLine(&line, static_cast<REAL>(guideView_.left), static_cast<REAL>(guideView_.top - t.s(6)),
               static_cast<REAL>(guideView_.right), static_cast<REAL>(guideView_.top - t.s(6)));
    if (height(guideFoot_) > 0)
        g.DrawLine(&line, static_cast<REAL>(guideFoot_.left), static_cast<REAL>(guideFoot_.top - t.s(4)),
                   static_cast<REAL>(guideFoot_.right), static_cast<REAL>(guideFoot_.top - t.s(4)));

    // The rows are drawn into a bitmap the size of the scrolling area and copied in, so nothing of a half-scrolled
    // row can land over the header or the link bar (GDI text ignores the clip a Graphics object holds).
    int viewW = width(guideView_), viewH0 = height(guideView_);
    Bitmap page(viewW, viewH0, PixelFormat32bppRGB);
    {
        Graphics cg(&page);
        SolidBrush pageBg(gc(t.c.background));
        cg.SetSmoothingMode(SmoothingModeNone);  // the fill must cover every pixel, edges included
        cg.FillRectangle(&pageBg, -1, -1, viewW + 2, viewH0 + 2);
        cg.SetSmoothingMode(SmoothingModeAntiAlias);
    int x = 0, w = viewW - t.s(4);
    for (const GuideRow& r : guideRows_) {
        int top = r.y - guideScroll_, bottom = top + r.h;
        if (bottom < 0 || top > viewH0) continue;
        const GuideBlock& b = guide_.blocks[r.block];
        switch (b.kind) {
            case Kind::Heading:
                drawText(cg, b.text, RECT{x, top, x + w, bottom}, t.heading, t.c.text, DT_SINGLELINE);
                break;
            case Kind::Text:
                drawText(cg, b.text, RECT{x, top, x + w, bottom}, t.body, t.c.text, DT_WORDBREAK);
                break;
            case Kind::Step: {
                // A numbered circle in neutral colors: red stays for the one button that starts the game.
                int d = t.s(26);
                RectF ring(static_cast<REAL>(x), static_cast<REAL>(top), static_cast<REAL>(d), static_cast<REAL>(d));
                SolidBrush fill(gc(t.c.surface));
                cg.FillEllipse(&fill, ring);
                Pen outline(gc(t.c.textMuted), t.sf(1.2));
                cg.DrawEllipse(&outline, ring);
                drawText(cg, std::to_wstring(b.number), RECT{x, top, x + d, top + d}, t.caption, t.c.text,
                         DT_SINGLELINE | DT_CENTER | DT_VCENTER);
                drawText(cg, b.text, RECT{x + t.s(40), top + t.s(2), x + w, bottom}, t.body, t.c.text, DT_WORDBREAK);
                break;
            }
            case Kind::Bullet: {
                SolidBrush dot(gc(t.c.textMuted));
                int lineH = textHeight(dc, t.body, L"Ag", 100, DT_SINGLELINE);
                cg.FillEllipse(&dot, static_cast<REAL>(x + t.s(4)), static_cast<REAL>(top + (lineH - t.s(6)) / 2 + t.s(1)),
                              t.sf(6), t.sf(6));
                drawText(cg, b.text, RECT{x + t.s(22), top, x + w, bottom}, t.body, t.c.text, DT_WORDBREAK);
                break;
            }
            case Kind::Key: {
                int kw = textWidth(dc, t.strong, b.text) + t.s(20);
                kw = std::min(kw, t.s(kKeyW) - t.s(8));
                RECT box{x, top, x + kw, top + t.s(26)};
                fillRound(cg, rf(box), t.sf(5), t.c.surfaceHover);
                strokeRound(cg, rf(box), t.sf(5), t.c.border, t.sf(1));
                drawText(cg, b.text, box, t.strong, t.c.text, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
                drawText(cg, b.detail, RECT{x + t.s(kKeyW) + t.s(12), top + t.s(2), x + w, bottom}, t.body, t.c.text,
                         DT_WORDBREAK);
                break;
            }
            case Kind::Fact:
                drawText(cg, b.text, RECT{x, top + t.s(2), x + t.s(kLabelW), bottom}, t.caption, t.c.textMuted, DT_WORDBREAK);
                drawText(cg, b.detail, RECT{x + t.s(kLabelW) + t.s(12), top, x + w, bottom}, t.body, t.c.text, DT_WORDBREAK);
                break;
        }
    }
    }
    g.DrawImage(&page, static_cast<INT>(guideView_.left), static_cast<INT>(guideView_.top));

    // A thin scroll indicator when there is more than fits.
    int viewH = height(guideView_);
    if (guideContentH_ > viewH) {
        int trackH = viewH, thumbH = std::max(t.s(28), trackH * viewH / guideContentH_);
        int maxScroll = guideContentH_ - viewH;
        int thumbY = guideView_.top + (trackH - thumbH) * guideScroll_ / std::max(1, maxScroll);
        fillRound(g, RectF(static_cast<REAL>(guideView_.right - t.s(4)), static_cast<REAL>(thumbY), t.sf(3),
                           static_cast<REAL>(thumbH)),
                  t.sf(1.5), mix(t.c.surface, t.c.textMuted, 0.7f));
    }
}

}  // namespace fl::ui
