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
    overview_ = buildGuide(cfg_, cfg_.games[gi], siteOf(gi));
    guide_ = overview_;
    pageTab_ = 0;
    guideScroll_ = 0;
    ShowWindow(grid_, SW_HIDE);
    relayout();
    InvalidateRect(hwnd_, &gridRc_, FALSE);
    if (guideBack_) SetFocus(guideBack_);
}

void Launcher::closeGuide() {
    if (!guidePage()) return;
    guideGame_ = -1;
    guide_ = overview_ = {};
    guideRows_.clear();
    relayout();
    ShowWindow(grid_, visible_.empty() || roomsPage() ? SW_HIDE : SW_SHOW);
    InvalidateRect(hwnd_, &gridRc_, FALSE);
}

// What the game page's details card lists: who made it, players, where it comes from, the pinned version, its state.
std::vector<std::pair<std::wstring, std::wstring>> Launcher::pageFacts(int gi) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const GameSpec& g = cfg_.games[gi];
    if (g.mod) out.push_back({cfg_.t("page.madeBy"), g.mod->mod.publisher.empty() ? cfg_.branding.title : g.mod->mod.publisher});
    if (!g.playerCount.empty()) out.push_back({cfg_.t("page.players"), g.playerCount});
    if (g.mod && !g.mod->mod.website.empty()) {
        std::wstring site = g.mod->mod.website;
        for (const wchar_t* scheme : {L"https://", L"http://"})
            if (site.rfind(scheme, 0) == 0) site = site.substr(wcslen(scheme));
        out.push_back({cfg_.t("page.source"), site});
    }
    if (g.mod && !g.mod->package.version.empty())
        out.push_back({cfg_.t("page.version"), g.mod->package.sha256.empty()
                                                   ? g.mod->package.version
                                                   : cfg_.t("page.pinned", {{L"VERSION", g.mod->package.version}})});
    else if (std::wstring v = verifiedVersion(gi); !v.empty())
        out.push_back({cfg_.t("page.version"), v});
    Color c;
    if (std::wstring st = stateText(gi, c); !st.empty()) out.push_back({cfg_.t("page.status"), st});
    return out;
}

// The mod version a download would be checked against by SHA-256: a community mod's pinned one, else the newest
// mods-info publishes (the package step refuses any it can't learn a hash for). Empty for Flash games.
std::wstring Launcher::verifiedVersion(int gi) const {
    const GameSpec& g = cfg_.games[gi];
    if (!g.mod) return {};
    if (!g.mod->package.sha256.empty() && !g.mod->package.version.empty()) return g.mod->package.version;
    const ModView* v = modView(gi);
    return v && v->source.published() ? v->source.version : std::wstring();
}

// The Details tab: the facts card's items and more, one per line, with the guide's links kept for the footer.
GuidePage Launcher::detailsPage(int gi) {
    GuidePage page;
    page.links = overview_.links;
    for (const auto& [label, value] : pageFacts(gi)) page.blocks.push_back({GuideBlock::Kind::Fact, 0, label, value});
    const GameSpec& g = cfg_.games[gi];
    if (std::wstring cats = categoryText(g); !cats.empty()) page.blocks.push_back({GuideBlock::Kind::Fact, 0, cfg_.t("page.categories"), cats});
    if (std::wstring played = statsText(user_.find(g.id)); !played.empty())
        page.blocks.push_back({GuideBlock::Kind::Fact, 0, cfg_.t("page.played"), played});
    return page;
}

void Launcher::showPageTab(int tab) {
    if (!guidePage() || tab == pageTab_) return;
    pageTab_ = tab;
    guide_ = tab == 0 ? overview_ : detailsPage(guideGame_);
    guideScroll_ = 0;
    relayout();
}

// The game page: a header over the game's art (Back, icon, kicker, title, tagline), a card of facts, and the guide
// in a card that scrolls. Rows are measured for the card's width; each keeps where it starts, so painting is a loop.
void Launcher::layoutGuidePage() {
    if (!guideBack_) return;
    std::vector<HWND> controls = {guideBack_, pageTabs_[0], pageTabs_[1]};
    for (HWND h : guideLinkBtns_) controls.push_back(h);
    if (!guidePage() || width(gridRc_) <= 0) {
        for (HWND h : controls) ShowWindow(h, SW_HIDE);
        guideRows_.clear();
        guideView_ = guideHead_ = guideFoot_ = guideHeader_ = guideIcon_ = guideFacts_ = guideCard_ = pageTabsRc_ = verified_ = {};
        return;
    }
    const Theme& t = theme_;
    HDC dc = GetDC(hwnd_);
    int pad = t.s(kPad + 8), x = gridRc_.left + pad, w = width(gridRc_) - pad * 2;

    // Header: Back on top, then the icon with the text block beside it.
    guideHeader_ = {gridRc_.left, gridRc_.top, gridRc_.right, gridRc_.top + t.s(204)};
    int backW = textWidth(dc, t.button, cfg_.t("guide.back")) + t.s(32), backH = t.s(32);
    SetWindowTextW(guideBack_, cfg_.t("guide.back").c_str());
    MoveWindow(guideBack_, x, gridRc_.top + t.s(14), backW, backH, TRUE);
    int iconS = t.s(92);
    guideIcon_ = {x, gridRc_.top + t.s(62), x + iconS, gridRc_.top + t.s(62) + iconS};
    guideHead_ = {guideIcon_.right + t.s(22), guideIcon_.top - t.s(2), x + w, guideHeader_.bottom - t.s(8)};
    int y = guideHeader_.bottom + t.s(2);

    // A pinned mod's version, checked by SHA-256 when it downloads, in the header's top right.
    verified_ = {};
    if (std::wstring version = verifiedVersion(guideGame_); !version.empty()) {
        int pw = textWidth(dc, t.caption, cfg_.t("page.verified", {{L"VERSION", version}})) + t.s(36);
        verified_ = {x + w - pw, gridRc_.top + t.s(18), x + w, gridRc_.top + t.s(18) + t.s(26)};
    }

    // The tabs, under the header.
    std::set<HWND> shown = {guideBack_};
    pageTabsRc_ = {x, y, x + w, y + t.s(42)};
    int tx = x;
    for (int i = 0; i < 2; ++i) {
        int tw = textWidth(dc, t.button, windowText(pageTabs_[i])) + t.s(32);
        MoveWindow(pageTabs_[i], tx, y, tw, height(pageTabsRc_), TRUE);
        if (btnInfo_[pageTabs_[i]].selected != (i == pageTab_)) {
            btnInfo_[pageTabs_[i]].selected = i == pageTab_;
            InvalidateRect(pageTabs_[i], nullptr, FALSE);
        }
        shown.insert(pageTabs_[i]);
        tx += tw + t.s(4);
    }
    y = pageTabsRc_.bottom + t.s(14);

    // The facts card leads the Overview; Details lists the same and more.
    std::vector<std::pair<std::wstring, std::wstring>> facts = pageFacts(guideGame_);
    guideFacts_ = {};
    if (!facts.empty() && pageTab_ == 0) {
        guideFacts_ = {x, y, x + w, y + t.s(68)};
        y = guideFacts_.bottom + t.s(16);
    }

    // Footer: one button per link.
    int footH = guide_.links.empty() ? 0 : t.s(48);
    guideFoot_ = {x, gridRc_.bottom - t.s(kPad) - footH, x + w, gridRc_.bottom - t.s(kPad)};
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
    guideCard_ = {x, y, x + w, guideFoot_.top - (footH ? t.s(8) : 0)};
    guideView_ = {guideCard_.left + t.s(24), guideCard_.top + t.s(18), guideCard_.right - t.s(18), guideCard_.bottom - t.s(18)};
    if (height(guideView_) < t.s(40)) guideView_.bottom = guideView_.top + t.s(40);

    // Rows.
    guideRows_.clear();
    int cy = 0, textW = width(guideView_) - t.s(4);
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
    const GameSpec& game = cfg_.games[guideGame_];
    const Color& bg = t.c.background;

    // Backdrop: the game's art behind the header, fading into the page below and darker on the text's side.
    int hw = width(guideHeader_), hh = height(guideHeader_);
    if (!pageBmp_ || pageKey_[0] != guideGame_ || pageKey_[1] != hw || pageKey_[2] != hh) {
        pageBmp_ = buildArt(guideGame_, hw, hh);
        pageKey_[0] = guideGame_;
        pageKey_[1] = hw;
        pageKey_[2] = hh;
    }
    RectF head = rf(guideHeader_);
    g.DrawImage(pageBmp_.get(), head.X, head.Y, head.Width, head.Height);
    LinearGradientBrush down(PointF(0, head.Y - 1), PointF(0, head.Y + head.Height + 1), gc(Color{bg.r, bg.g, bg.b, 120}),
                             gc(Color{bg.r, bg.g, bg.b, 255}));
    g.FillRectangle(&down, head);
    LinearGradientBrush across(PointF(head.X - 1, 0), PointF(head.X + head.Width + 1, 0), gc(Color{bg.r, bg.g, bg.b, 200}),
                               gc(Color{bg.r, bg.g, bg.b, 0}));
    g.FillRectangle(&across, head);

    // Icon: the same art, square, in a rounded frame.
    int iw = width(guideIcon_);
    if (!iconBmp_ || iconKey_[0] != guideGame_ || iconKey_[1] != iw) {
        iconBmp_ = buildArt(guideGame_, iw, iw);
        iconKey_[0] = guideGame_;
        iconKey_[1] = iw;
        iconKey_[2] = iw;
    }
    {
        GraphicsPath path;
        roundRectPath(path, rf(guideIcon_), t.sf(16));
        GraphicsState saved = g.Save();
        g.SetClip(&path, CombineModeIntersect);
        g.DrawImage(iconBmp_.get(), static_cast<REAL>(guideIcon_.left), static_cast<REAL>(guideIcon_.top), static_cast<REAL>(iw),
                    static_cast<REAL>(iw));
        g.Restore(saved);
        strokeRound(g, rf(guideIcon_), t.sf(16), mix(t.c.border, t.c.text, 0.15f), t.sf(1));
    }

    // Text beside the icon: kicker (the categories), title, tagline.
    RECT kr{guideHead_.left, guideHead_.top, guideHead_.right, guideHead_.top + t.s(18)};
    drawText(g, categoryText(game), kr, t.tiny, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT tr{guideHead_.left, kr.bottom + t.s(2), guideHead_.right, kr.bottom + t.s(46)};
    drawText(g, displayName(game), tr, t.title, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT dr{guideHead_.left, tr.bottom + t.s(4), std::min<LONG>(guideHead_.right, guideHead_.left + t.s(720)), guideHead_.bottom};
    HDC mdc = g.GetHDC();
    std::wstring tagline = fitText(mdc, t.body, game.description, width(dr), height(dr));
    g.ReleaseHDC(mdc);
    drawText(g, tagline, dr, t.body, mix(t.c.textMuted, t.c.text, 0.5f), DT_WORDBREAK);

    // The tabs sit on a rule across the page.
    if (height(pageTabsRc_) > 0) {
        Pen rule(gc(t.c.border), 1);
        g.DrawLine(&rule, static_cast<REAL>(pageTabsRc_.left), static_cast<REAL>(pageTabsRc_.bottom - 1),
                   static_cast<REAL>(pageTabsRc_.right), static_cast<REAL>(pageTabsRc_.bottom - 1));
    }
    if (height(verified_) > 0) {
        const Color& ok = t.c.success;
        fillRound(g, rf(verified_), t.sf(13), mix(t.c.surface, ok, 0.16f));
        strokeRound(g, rf(verified_), t.sf(13), mix(t.c.surface, ok, 0.5f), t.sf(1));
        SolidBrush dot(gc(ok));
        g.FillEllipse(&dot, static_cast<REAL>(verified_.left + t.s(12)), static_cast<REAL>((verified_.top + verified_.bottom) / 2 - t.s(4)),
                      t.sf(8), t.sf(8));
        RECT vt{verified_.left + t.s(26), verified_.top, verified_.right - t.s(8), verified_.bottom};
        // Light themes pull the green toward the text color to keep 4.5:1 on the tint.
        drawText(g, cfg_.t("page.verified", {{L"VERSION", verifiedVersion(guideGame_)}}), vt, t.caption, t.dark ? ok : mix(ok, t.c.text, 0.55f),
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }

    // Facts: label over value, in even columns.
    if (height(guideFacts_) > 0) {
        fillRound(g, rf(guideFacts_), t.sf(9), t.c.surface);
        strokeRound(g, rf(guideFacts_), t.sf(9), t.c.border, t.sf(1));
        std::vector<std::pair<std::wstring, std::wstring>> facts = pageFacts(guideGame_);
        int fx = guideFacts_.left + t.s(24), fw = (width(guideFacts_) - t.s(48)) / std::max<int>(1, static_cast<int>(facts.size()));
        for (const auto& [label, value] : facts) {
            drawText(g, label, RECT{fx, guideFacts_.top + t.s(14), fx + fw - t.s(16), guideFacts_.top + t.s(32)}, t.tiny,
                     t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            drawText(g, value, RECT{fx, guideFacts_.top + t.s(34), fx + fw - t.s(16), guideFacts_.bottom - t.s(10)}, t.caption,
                     t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            fx += fw;
        }
    }

    // The guide's card.
    fillRound(g, rf(guideCard_), t.sf(9), t.c.surface);
    strokeRound(g, rf(guideCard_), t.sf(9), t.c.border, t.sf(1));

    // The rows are drawn into a bitmap the size of the scrolling area and copied in, so nothing of a half-scrolled
    // row can land over the header or the link bar (GDI text ignores the clip a Graphics object holds).
    int viewW = width(guideView_), viewH0 = height(guideView_);
    Bitmap page(viewW, viewH0, PixelFormat32bppRGB);
    {
        Graphics cg(&page);
        SolidBrush pageBg(gc(t.c.surface));
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
