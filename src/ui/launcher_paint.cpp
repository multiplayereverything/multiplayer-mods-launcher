// The launcher window: painting (sidebar, cards, details panel, buttons, the opening entrance).
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

// ---- painting -------------------------------------------------------------

std::wstring Launcher::stateText(int gi, Color& color) const {
    if (cfg_.games[gi].mod) return modStateText(gi, color);
    if (cfg_.games[gi].comingSoon) {
        color = theme_.c.textMuted;
        return cfg_.t("status.comingSoon");
    }
    if (isPlaying(gi)) {
        color = theme_.c.online;
        return cfg_.t("status.playing");
    }
    switch (lib_.gameState(cfg_.games[gi], user_)) {
        case ActionState::Ready:
            if (!outdatedFor(cfg_.games[gi]).empty()) {
                color = theme_.c.online;
                return cfg_.t("status.update");
            }
            color = theme_.c.success;
            return cfg_.t("status.ready");
        case ActionState::NoPlayer: color = theme_.c.error; return cfg_.t("status.noPlayer");
        case ActionState::NoRuntime:
            color = theme_.c.error;
            for (const auto& a : cfg_.games[gi].actions)
                if (a.server.enabled()) return cfg_.t("status.noRuntime", {{L"RUNTIME", a.server.name}});
            return cfg_.t("status.noPlayer");
        case ActionState::NoSwf: break;
    }
    color = theme_.c.warning;
    return cfg_.t("status.noSwf");
}

void Launcher::ensureImage(int gi) {
    if (imageTried_[gi]) return;
    imageTried_[gi] = true;
    const GameSpec& g = cfg_.games[gi];
    if (g.image.empty()) return;
    std::wstring rel = lib_.vars().expand(g.image);
    std::wstring path = fs::isAbsolute(rel) ? rel : fs::join(cfg_.configDir, rel);
    std::string bytes;
    if (!fs::isFile(path) && !fs::isAbsolute(rel) && readEmbedded(rel, bytes)) images_[gi] = loadImageBytes(bytes);
    else images_[gi] = loadImageFile(path);
}

int Launcher::heroCandidate() const {
    int best = -1;
    int64_t when = 0;
    for (size_t i = 0; i < cfg_.games.size(); ++i) {
        if (cfg_.games[i].comingSoon) continue;
        const GameStats* st = user_.find(cfg_.games[i].id);
        if (st && st->lastPlayed > when) {
            when = st->lastPlayed;
            best = static_cast<int>(i);
        }
    }
    return best;
}

// Shown over the plain grid only: not on the Rooms or Guide pages, not while searching, not in the narrow layout.
bool Launcher::heroWanted() const {
    return !isNarrow() && !roomsPage() && !guidePage() && GetWindowTextLengthW(search_) == 0 && heroCandidate() >= 0;
}

void Launcher::paintHero(Graphics& g) {
    if (IsRectEmpty(&hero_) || heroGame_ < 0 || roomsPage() || guidePage()) return;
    const Theme& t = theme_;
    const GameSpec& game = cfg_.games[heroGame_];
    int w = width(hero_), h = height(hero_);
    RectF box = rf(hero_);
    float radius = t.sf(14);
    if (!heroBmp_ || heroBmpGame_ != heroGame_ || heroBmpW_ != w || heroBmpH_ != h) {
        ensureImage(heroGame_);
        if (images_[heroGame_]) {
            heroBmp_ = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
            Graphics gg(heroBmp_.get());
            gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            drawCover(gg, *images_[heroGame_], RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)), 0, true, game.imageFocus);
        } else {
            heroBmp_ = coverArt(displayName(game), game.color, game.colorEnd, w, h);
        }
        heroBmpGame_ = heroGame_;
        heroBmpW_ = w;
        heroBmpH_ = h;
    }
    GraphicsPath path;
    roundRectPath(path, box, radius);
    GraphicsState saved = g.Save();
    g.SetClip(&path, CombineModeIntersect);
    g.DrawImage(heroBmp_.get(), box.X, box.Y, box.Width, box.Height);
    // The left side darkens so the title keeps its contrast over any picture.
    const Color& sb = t.c.sidebar;
    LinearGradientBrush shade(PointF(box.X, 0), PointF(box.X + box.Width, 0), gc(Color{sb.r, sb.g, sb.b, 238}),
                              gc(Color{sb.r, sb.g, sb.b, 0}));
    g.FillRectangle(&shade, box);
    g.Restore(saved);
    strokeRound(g, box, radius, t.c.border, t.sf(1));

    int x = hero_.left + t.s(24);
    HDC mdc = g.GetHDC();
    std::wstring chip = cfg_.t("hero.continue");
    int cw = textWidth(mdc, t.tiny, chip) + t.s(16);
    g.ReleaseHDC(mdc);
    RECT chipR{x, hero_.top + t.s(16), x + cw, hero_.top + t.s(16) + t.s(20)};
    fillRound(g, rf(chipR), t.sf(10), Color{sb.r, sb.g, sb.b, 225});
    drawText(g, chip, chipR, t.tiny, t.c.sidebarText, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    const GameStats* st = user_.find(game.id);
    RECT whenR{chipR.right + t.s(10), chipR.top, hero_.right - t.s(24), chipR.bottom};
    drawText(g, cfg_.t("hero.played", {{L"WHEN", formatAgo(st ? st->lastPlayed : 0)}}), whenR, t.caption, t.c.text,
             DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    RECT nameR{x, chipR.bottom + t.s(8), hero_.left + w * 6 / 10, chipR.bottom + t.s(8) + t.s(40)};
    drawText(g, displayName(game), nameR, t.title, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

Bitmap* Launcher::art(int gi, int w, int h, bool detail) {
    auto key = std::make_pair(gi, detail);
    auto it = artCache_.find(key);
    if (it != artCache_.end() && it->second && static_cast<int>(it->second->GetWidth()) == w &&
        static_cast<int>(it->second->GetHeight()) == h)
        return it->second.get();
    const GameSpec& g = cfg_.games[gi];
    ensureImage(gi);
    std::unique_ptr<Bitmap> bmp;
    if (images_[gi]) {
        // Pre-scale once so painting is a 1:1 blit (smooth scrolling).
        bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
        Graphics gg(bmp.get());
        gg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        drawCover(gg, *images_[gi], RectF(0, 0, static_cast<REAL>(w), static_cast<REAL>(h)), 0, true, g.imageFocus);
    } else {
        bmp = coverArt(displayName(g), g.color, g.colorEnd, w, h);
    }
    Bitmap* raw = bmp.get();
    artCache_[key] = std::move(bmp);
    return raw;
}

void Launcher::paint(HDC dc, const RECT& dirty) {
    RECT cr;
    GetClientRect(hwnd_, &cr);
    Buffer buf(dc, cr.right, cr.bottom);
    if (intro_) {
        {
            Graphics g(buf.dc);
            paintIntro(g);
        }
        BitBlt(dc, 0, 0, cr.right, cr.bottom, buf.dc, 0, 0, SRCCOPY);
        return;
    }
    {
        Graphics g(buf.dc);
        // Animation frames invalidate a few small rectangles; skip the rest.
        g.SetClip(rf(dirty));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        const Theme& t = theme_;
        SolidBrush bg(gc(t.c.background));
        g.FillRectangle(&bg, 0, 0, cr.right, cr.bottom);
        paintSidebar(g, buf.dc);

        // Search box.
        fillRound(g, rf(searchBox_), t.sf(6), t.c.surface);
        strokeRound(g, rf(searchBox_), t.sf(6), GetFocus() == search_ ? t.c.primary : t.c.border, t.sf(1));
        RECT icon{searchBox_.left + t.s(10), searchBox_.top, searchBox_.left + t.s(30), searchBox_.bottom};
        drawText(g, L"\u2315", icon, t.body, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        paintHero(g);
        if (roomsPage()) {
            paintRoomsPage(g);
        } else if (guidePage()) {
            paintGuidePage(g, buf.dc);
        } else if (visible_.empty()) {
            RECT r = gridRc_;
            r.top += t.s(80);
            drawText(g, emptyTitle(), r, t.heading, t.c.text, DT_CENTER | DT_SINGLELINE);
            r.top += t.s(30);
            drawText(g, emptyBody(), r, t.body, t.c.textMuted, DT_CENTER | DT_SINGLELINE);
        }
        paintDetail(g, buf.dc);
        paintStatus(g, buf.dc);
    }
    BitBlt(dc, dirty.left, dirty.top, width(dirty), height(dirty), buf.dc, dirty.left, dirty.top, SRCCOPY);
}

void Launcher::paintSidebar(Graphics& g, HDC) {
    if (width(sidebar_) <= 0) return;
    const Theme& t = theme_;
    SolidBrush sb(gc(t.c.sidebar));
    g.FillRectangle(&sb, rf(sidebar_));
    // Text-only brand: no logo has been supplied, so none is invented.
    RECT r1{t.s(24), t.s(20), sidebar_.right - t.s(12), t.s(48)};
    drawText(g, cfg_.branding.title, r1, t.brand, t.c.sidebarText, DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT r2{t.s(24), t.s(48), sidebar_.right - t.s(12), t.s(70)};
    drawText(g, cfg_.branding.subtitle, r2, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Launcher::paintDetail(Graphics& g, HDC dc) {
    const Theme& t = theme_;
    SolidBrush panel(gc(t.c.surface));
    g.FillRectangle(&panel, rf(detail_));
    Pen edge(gc(t.c.border), 1);
    g.DrawLine(&edge, static_cast<REAL>(detail_.left), 0.0f, static_cast<REAL>(detail_.left),
               static_cast<REAL>(detail_.bottom));
    if (selected_ < 0) {
        RECT r = detail_;
        r.top += t.s(80);
        drawText(g, cfg_.t("detail.empty"), r, t.body, t.c.textMuted, DT_CENTER | DT_SINGLELINE);
        return;
    }
    const GameSpec& game = cfg_.games[selected_];
    if (height(dl_.art) > 0)
        if (Bitmap* a = art(selected_, width(dl_.art), height(dl_.art), true))
            drawCover(g, *a, rf(dl_.art), t.sf(8), true);
    if (height(dl_.title) > 0) {
        std::wstring title = displayName(game);
        if (ULONGLONG e = since(titleFrom_); e < kTitleMs)
            title = motion::scramble(title, static_cast<float>(e) / kTitleMs, static_cast<uint32_t>(e / 50));
        drawText(g, title, dl_.title, t.title, t.c.text, DT_WORDBREAK);
    }
    if (height(dl_.meta) > 0) drawText(g, categoryText(game), dl_.meta, t.caption, t.c.textMuted, DT_WORDBREAK);
    if (height(dl_.slots) > 0) paintSlots(g, game);

    // Status pill.
    if (height(dl_.status) > 0) {
        Color sc;
        std::wstring status = stateText(selected_, sc);
        HDC mdc = g.GetHDC();
        int tw = textWidth(mdc, t.caption, status);
        g.ReleaseHDC(mdc);
        RECT pill{dl_.status.left, dl_.status.top, dl_.status.left + tw + t.s(30), dl_.status.bottom};
        fillRound(g, rf(pill), t.sf(12), mix(t.c.surface, sc, 0.12f));
        SolidBrush dot(gc(sc));
        g.FillEllipse(&dot, static_cast<REAL>(pill.left + t.s(10)),
                      static_cast<REAL>(pill.top + height(pill) / 2 - t.s(4)), t.sf(8), t.sf(8));
        RECT pt{pill.left + t.s(22), pill.top, pill.right, pill.bottom};
        drawText(g, status, pt, t.caption, sc, DT_SINGLELINE | DT_VCENTER);
    }

    if (height(dl_.site) > 0)
        if (const SiteGame* info = siteOf(selected_)) paintSite(g, dc, dl_.site, *info, game.comingSoon);
    if (height(dl_.desc) > 0) drawText(g, descText_, dl_.desc, t.body, t.c.text, DT_WORDBREAK);
    std::wstring stats = statsText(user_.find(game.id));
    if (height(dl_.stats) > 0) drawText(g, stats, dl_.stats, t.caption, t.c.textMuted, DT_WORDBREAK);
    if (width(dl_.playerLabel) > 0)
        drawText(g, cfg_.t("detail.player"), dl_.playerLabel, t.strong, t.c.text, DT_SINGLELINE | DT_VCENTER);
    for (const auto& [rc, text] : dl_.settingLabels) drawText(g, text, rc, t.strong, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    if (width(dl_.saveLabel) > 0)
        drawText(g, cfg_.t("coopSave.label"), dl_.saveLabel, t.strong, t.c.text, DT_SINGLELINE | DT_VCENTER);
    if (height(dl_.saveNote) > 0)
        drawText(g, dl_.saveNoteText, dl_.saveNote, t.caption, dl_.saveNoteWarn ? t.c.warning : t.c.textMuted, DT_WORDBREAK);

    if (height(dl_.tabs) > 0) {
        Pen line(gc(t.c.border), 1);
        g.DrawLine(&line, static_cast<REAL>(dl_.tabs.left), static_cast<REAL>(dl_.tabs.bottom - 1),
                   static_cast<REAL>(dl_.tabs.right), static_cast<REAL>(dl_.tabs.bottom - 1));
    }
    if (detailTab_ == 1 && height(dl_.rooms) > 0) {
        for (const auto& row : tabRows_) paintRoomRow(g, row, false, t.s(64));
        if (tabRows_.empty()) {
            RECT er = dl_.rooms;
            er.top += t.s(8);
            const char* why = !roomsKnown_ ? "roomsPage.offline" : notOnServer(game) ? "rooms.notOnServer" : "rooms.none";
            drawText(g, cfg_.t(why), er, t.caption, t.c.textMuted, DT_WORDBREAK);
        }
    }
    if (height(dl_.codeBox) > 0) {
        fillRound(g, rf(dl_.codeBox), t.sf(6), t.c.background);
        strokeRound(g, rf(dl_.codeBox), t.sf(6), GetFocus() == tabCodeEdit_ ? t.c.focus : t.c.border, t.sf(1));
    }

    if (height(dl_.host) > 0 && !dl_.hostLines.empty()) {
        fillRound(g, rf(dl_.host), t.sf(6), mix(t.c.surface, t.c.online, 0.10f));
        strokeRound(g, rf(dl_.host), t.sf(6), mix(t.c.surface, t.c.online, 0.55f), t.sf(1));
        std::vector<HostLine> view = hostView();
        for (size_t i = 0; i < view.size() && i < dl_.hostLines.size(); ++i) {
            HFONT f = view[i].role == 0 ? t.button : t.caption;
            const Color& c = view[i].role == 0 ? t.c.online : view[i].role == 1 ? t.c.textMuted : t.c.text;
            drawText(g, view[i].text, dl_.hostLines[i], f, c, DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
}

void Launcher::paintStatus(Graphics& g, HDC) {
    const Theme& t = theme_;
    SolidBrush bar(gc(t.c.sidebar));
    g.FillRectangle(&bar, rf(status_));
    RECT r{status_.left + t.s(16), status_.top, status_.right - t.s(16), status_.bottom};
    // Right: which players were found.
    std::wstring right;
    for (const auto& p : lib_.players())
        right += (right.empty() ? L"" : L"   ") +
                 cfg_.t(p.found() ? "bar.found" : "bar.missing", {{L"PLAYER", p.spec->name}});
    if (!lib_.flashpointDir().empty()) right += L"   " + cfg_.t("bar.flashpoint", {{L"PATH", lib_.flashpointDir()}});
    HDC mdc = g.GetHDC();
    int rw = std::min<int>(textWidth(mdc, t.caption, right), width(r) / 2);
    Color serverColor;
    std::wstring server = serverText(serverColor);
    int sw = server.empty() ? 0 : textWidth(mdc, t.caption, server);
    g.ReleaseHDC(mdc);
    RECT rr{r.right - rw, r.top, r.right, r.bottom};
    drawText(g, right, rr, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
    if (sw && rr.left - sw - t.s(40) > r.left + width(r) / 3) {
        RECT sr{rr.left - t.s(24) - sw, r.top, rr.left - t.s(24), r.bottom};
        SolidBrush dot(gc(serverColor));
        g.FillEllipse(&dot, static_cast<REAL>(sr.left - t.s(13)), static_cast<REAL>((sr.top + sr.bottom) / 2 - t.s(4)),
                      t.sf(8), t.sf(8));
        drawText(g, server, sr, t.caption, serverColor, DT_SINGLELINE | DT_VCENTER);
        rr.left = sr.left - t.s(13);
    }

    RECT left{r.left, r.top, rr.left - t.s(16), r.bottom};
    std::wstring text;
    Color color = t.c.sidebarText;
    if (!notice_.empty() && GetTickCount64() < noticeUntil_) {
        text = notice_;
        color = noticeColor_;
    } else if (installing_) {
        text = installText();
        color = t.c.online;
    } else if (!sessions_.empty()) {
        const Session& s = sessions_.back();
        ULONGLONG secs = (GetTickCount64() - s.started) / 1000;
        wchar_t clock[32];
        swprintf(clock, 32, L"%llu:%02llu", secs / 60, secs % 60);
        text = cfg_.t("bar.playing", {{L"GAME", displayName(cfg_.games[s.game]) + L" (" + s.player + L")"}, {L"TIME", clock}});
        color = t.c.online;
    }
    if (!text.empty()) {
        SolidBrush dot(gc(color));
        g.FillEllipse(&dot, static_cast<REAL>(left.left), static_cast<REAL>((left.top + left.bottom) / 2 - t.s(4)), t.sf(8),
                      t.sf(8));
        left.left += t.s(16);
        drawText(g, text, left, t.caption, color, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

void Launcher::drawCard(HDC dc, int gi, RECT tile, bool selected, bool focused) {
    const Theme& t = theme_;
    const GameSpec& game = cfg_.games[gi];
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush bg(gc(t.c.background));
    g.SetSmoothingMode(SmoothingModeNone);  // see drawButton
    g.FillRectangle(&bg, rf(tile));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    int half = t.s(kGap) / 2;
    RECT c{tile.left + half, tile.top + half, tile.right - half, tile.bottom - half};
    bool hover = hoverItem_ >= 0 && hoverItem_ < static_cast<int>(visible_.size()) && visible_[hoverItem_] == gi;
    // Under the mouse the card rises a little and takes its game's color on the edge.
    float lift = motion_ ? (lift_.count(gi) ? lift_[gi] : 0.0f) : (hover ? 1.0f : 0.0f);
    OffsetRect(&c, 0, -static_cast<int>(t.s(3) * lift + 0.5f));
    float radius = t.sf(12);
    fillRound(g, rf(c), radius, hover || selected ? t.c.surfaceHover : t.c.surface);
    int artH = static_cast<int>(width(c) * 0.5625);
    RECT artR{c.left, c.top, c.right, c.top + artH};
    if (Bitmap* a = art(gi, width(artR), height(artR), false)) drawCover(g, *a, rf(artR), radius, false);

    // Badges on the art.
    auto badge = [&](const std::wstring& text, const Color& textColor, bool right, int dx = 0) {
        HDC mdc = g.GetHDC();
        int w = textWidth(mdc, t.tiny, text) + t.s(14);
        g.ReleaseHDC(mdc);
        int bh = t.s(20);
        RECT b = right ? RECT{artR.right - t.s(10) - dx - w, artR.top + t.s(10), artR.right - t.s(10) - dx, artR.top + t.s(10) + bh}
                       : RECT{artR.left + t.s(10) + dx, artR.top + t.s(10), artR.left + t.s(10) + dx + w, artR.top + t.s(10) + bh};
        fillRound(g, rf(b), t.sf(10), Color{t.c.sidebar.r, t.c.sidebar.g, t.c.sidebar.b, 225});
        drawText(g, text, b, t.tiny, textColor, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
        return w;
    };
    bool online = !game.onlineId.empty();
    for (const auto& a : game.actions) online = online || a.relay.enabled;
    int usedLeft = online ? badge(cfg_.t("badge.online"), t.c.sidebarText, false) + t.s(6) : 0;
    if (game.mod) badge(cfg_.t("badge.pc"), t.c.sidebarText, false, usedLeft);
    const GameStats* st = user_.find(game.id);
    // The website's quality grade sits in the top right; a favorite's star moves left of it.
    int chipW = 0;
    if (const SiteGame* info = siteOf(gi); info && !info->quality.empty() && cfg_.text.count("site.quality." + info->quality)) {
        int cs = t.s(22);
        RECT chip{artR.right - t.s(10) - cs, artR.top + t.s(9), artR.right - t.s(10), artR.top + t.s(9) + cs};
        paintGradeChip(g, chip, info->quality);
        chipW = cs + t.s(6);
    }
    if (st && st->favorite) badge(L"\u2605", t.c.warning, true, chipW);

    int x = c.left + t.s(14), w = width(c) - t.s(28), y = artR.bottom + t.s(10);
    RECT nameR{x, y, x + w, y + t.s(26)};
    drawText(g, displayName(game), nameR, t.name, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    y += t.s(28);
    // One block per player slot, in the players' own colors.
    int pip = t.s(10), pipGap = t.s(3), px = x;
    const auto& colors = cfg_.branding.playerColors;
    for (int i = 0; i < game.maxPlayers && !colors.empty(); ++i) {
        fillRound(g, RectF(static_cast<REAL>(px), static_cast<REAL>(y + t.s(5)), static_cast<REAL>(pip),
                           static_cast<REAL>(pip)),
                  t.sf(2), colors[i % colors.size()]);
        px += pip + pipGap;
    }
    RECT mr{px + (game.maxPlayers ? t.s(6) : 0), y, x + w, y + t.s(20)};
    drawText(g, game.playerCount, mr, t.caption, t.c.textMuted, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    y += t.s(24);
    Color sc;
    std::wstring status = stateText(gi, sc);
    RECT sr{x, y, x + w, y + t.s(20)};
    // Live rooms on the central server, shown only when there are some.
    std::wstring roomsText = notOnServer(game) ? cfg_.t("status.notOnServer") : roomCountText(game.id);
    if (!roomsText.empty()) {
        HDC mdc = g.GetHDC();
        int rw = textWidth(mdc, t.caption, roomsText);
        g.ReleaseHDC(mdc);
        RECT rr{std::max<LONG>(x, x + w - rw), y, x + w, y + t.s(20)};
        drawText(g, roomsText, rr, t.caption, notOnServer(game) ? t.c.textMuted : t.c.online,
                 DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        sr.right = rr.left - t.s(8);
    }
    drawText(g, status, sr, t.caption, sc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    // Selection is a neutral outline: red means "start", blue means keyboard focus.
    if (!selected && lift > 0.01f) strokeRound(g, rf(c), radius, mix(t.c.surface, game.color, 0.85f * lift), t.sf(1.5));
    if (selected) strokeRound(g, rf(c), radius, t.c.text, t.sf(2));
    if (focused && !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
        RECT f{c.left - t.s(3), c.top - t.s(3), c.right + t.s(3), c.bottom + t.s(3)};
        strokeRound(g, rf(f), radius + t.sf(3), t.c.focus, t.sf(1.5));
    }
}

void Launcher::drawButton(HWND button, HDC hdc, RECT r, UINT state) {
    // Custom draw can report a rectangle smaller than the control, which leaves a
    // line of the default look along an edge; always cover the whole client area.
    GetClientRect(button, &r);
    const Theme& t = theme_;
    const BtnInfo& info = btnInfo_[button];
    Buffer buf(hdc, width(r), height(r));
    {
        Graphics g(buf.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // Buttons live on the detail panel (or the hosting box inside it).
        RECT onScreen;
        GetWindowRect(button, &onScreen);
        MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&onScreen), 2);
        POINT mid{(onScreen.left + onScreen.right) / 2, (onScreen.top + onScreen.bottom) / 2};
        Color under = t.c.surface;  // the details panel and the Settings window
        if (GetParent(button) == hwnd_) {
            if (PtInRect(&dl_.host, mid)) under = mix(t.c.surface, t.c.online, 0.10f);
            else if (PtInRect(&sidebar_, mid)) under = t.c.sidebar;
            else if (!PtInRect(&detail_, mid)) under = t.c.background;
        }
        SolidBrush ub(gc(under));
        // Flat fills of the whole buffer must not be anti-aliased: half-covered edge
        // pixels would mix with the blank (black) buffer into a dark line.
        g.SetSmoothingMode(SmoothingModeNone);
        g.FillRectangle(&ub, 0, 0, width(r), height(r));
        g.SetSmoothingMode(SmoothingModeAntiAlias);

        bool hover = hoverBtn_ == button, pressed = (state & CDIS_SELECTED) != 0;
        Color fill = t.c.surfaceHover, text = t.c.text, border = t.c.border;
        if (info.style == BtnStyle::Tab) {
            // A tab: plain text, the open one bold with a bar under it.
            std::wstring label = windowText(button);
            RECT tr{0, 0, width(r), height(r) - t.s(3)};
            drawText(g, label, tr, t.button, info.selected || hover ? t.c.text : t.c.textMuted,
                     DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
            if (info.selected) {
                RectF bar(t.sf(10), static_cast<REAL>(height(r)) - t.sf(3), static_cast<REAL>(width(r)) - t.sf(20), t.sf(3));
                fillRound(g, bar, t.sf(1.5), t.c.text);
            }
            if ((state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RectF f(t.sf(2), t.sf(2), static_cast<REAL>(width(r)) - t.sf(4), static_cast<REAL>(height(r)) - t.sf(6));
                strokeRound(g, f, t.sf(6), t.c.focus, t.sf(1.5));
            }
        } else {
        switch (info.style) {
            case BtnStyle::Primary:
                // A primary action that can't start looks like a secondary one, so its
                // muted label keeps AA contrast.
                fill = info.ready ? (hover ? t.c.primaryHover : t.c.primary)
                                  : (hover ? mix(t.c.surfaceHover, t.c.text, 0.06f) : t.c.surfaceHover);
                text = info.ready ? t.c.onPrimary : t.c.textMuted;
                border = info.ready ? fill : t.c.border;
                break;
            case BtnStyle::Secondary:
                fill = hover ? mix(t.c.surfaceHover, t.c.text, 0.06f) : t.c.surfaceHover;
                text = info.ready ? t.c.text : t.c.textMuted;
                break;
            case BtnStyle::Ghost:
                fill = hover ? t.c.surfaceHover : under;
                border = hover ? t.c.border : under;
                break;
            case BtnStyle::Link:
                // A page of someone else's (the mod's repository): amber, so it is not mistaken for an action
                // of ours. The label is pulled toward the text color to keep 4.5:1 on the tint in both themes.
                fill = hover ? mix(t.c.surfaceHover, t.c.warning, 0.22f) : mix(t.c.surfaceHover, t.c.warning, 0.15f);
                text = mix(t.c.warning, t.c.text, 0.55f);
                border = mix(t.c.surfaceHover, t.c.warning, 0.5f);
                break;
            case BtnStyle::Danger:
                fill = hover ? mix(t.c.surfaceHover, t.c.error, 0.22f) : mix(t.c.surfaceHover, t.c.error, 0.15f);
                text = mix(t.c.error, Color{255, 255, 255, 255}, 0.35f);  // plain error red is under 4.5:1 on the tint
                border = mix(t.c.surfaceHover, t.c.error, 0.4f);
                break;
        }
        if (info.style == BtnStyle::Check) {
            // A checkbox: box on the left, label beside it. Checked fills with the
            // focus blue and a check mark in the background color.
            bool checked = SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
            int bs = t.s(18), by = (height(r) - bs) / 2;
            RectF cb(t.sf(1), static_cast<REAL>(by), static_cast<REAL>(bs), static_cast<REAL>(bs));
            if (checked) {
                fillRound(g, cb, t.sf(4), hover ? mix(t.c.focus, t.c.text, 0.25f) : t.c.focus);
                Pen mark(gc(t.c.background), t.sf(2.2));
                mark.SetStartCap(LineCapRound);
                mark.SetEndCap(LineCapRound);
                PointF pts[] = {PointF(cb.X + bs * 0.24f, cb.Y + bs * 0.52f), PointF(cb.X + bs * 0.43f, cb.Y + bs * 0.7f),
                                PointF(cb.X + bs * 0.77f, cb.Y + bs * 0.3f)};
                g.DrawLines(&mark, pts, 3);
            } else {
                strokeRound(g, cb, t.sf(4), hover ? t.c.text : t.c.textMuted, t.sf(1.5));
            }
            RECT lr{bs + t.s(10), 0, width(r), height(r)};
            drawText(g, windowText(button), lr, t.body, t.c.text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if ((state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
                RectF f(0.5f, 0.5f, static_cast<REAL>(width(r)) - 1, static_cast<REAL>(height(r)) - 1);
                strokeRound(g, f, t.sf(6), t.c.focus, t.sf(1.5));
            }
        } else {
        // Pressed, the face sinks a little and darkens (FlutterFX ShimmerButton).
        int sink = pressed ? t.s(1) : 0;
        if (pressed) fill = mix(fill, Color{0, 0, 0, 255}, 0.15f);
        RectF box(0.5f, 0.5f + sink, static_cast<REAL>(width(r)) - 1, static_cast<REAL>(height(r)) - 1 - sink);
        fillRound(g, box, t.sf(6), fill);
        strokeRound(g, box, t.sf(6), border, t.sf(1));

        std::wstring label = info.label.empty() ? windowText(button) : info.label;
        if (!IsWindowEnabled(button)) text = t.c.textMuted;
        RECT tr{t.s(14), sink, width(r) - t.s(14), height(r) + sink};
        auto iconAt = [&](RECT ir) {  // the icon glyph; returns its width
            if (info.icon.empty() || !t.icon) return 0;
            drawText(g, info.icon, ir, t.icon, text, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            return t.s(26);
        };
        if (!info.sub.empty() && height(r) >= t.s(48)) {
            RECT top{tr.left, t.s(8) + sink, tr.right, height(r) / 2 + t.s(2) + sink};
            RECT bottom{tr.left, height(r) / 2 + t.s(1) + sink, tr.right, height(r) - t.s(6) + sink};
            int iw = iconAt(top);
            top.left += iw;
            if (!info.tag.empty()) {
                // A small pill after the label: "Original".
                HDC mdc = g.GetHDC();
                int lw = textWidth(mdc, t.button, label), pw = textWidth(mdc, t.tiny, info.tag) + t.s(12);
                g.ReleaseHDC(mdc);
                int px = std::min<int>(top.left + lw + t.s(8), top.right - pw);
                RECT pill{px, (top.top + top.bottom) / 2 - t.s(9), px + pw, (top.top + top.bottom) / 2 + t.s(9)};
                strokeRound(g, rf(pill), t.sf(9), mix(fill, text, 0.5f), t.sf(1));
                drawText(g, info.tag, pill, t.tiny, text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
                top.right = pill.left - t.s(4);
            }
            drawText(g, label, top, t.button, text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            Color subColor = info.style == BtnStyle::Primary && info.ready ? t.c.onPrimary
                             : info.style == BtnStyle::Link                 ? t.c.text
                             : info.ready                                   ? t.c.textMuted
                                                                            : t.c.warning;
            drawText(g, info.sub, bottom, t.caption, subColor, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            if (info.style == BtnStyle::Primary && info.ready) {
                RECT arrow{tr.right - t.s(20), 0, tr.right, height(r)};
                drawText(g, L"\u25B6", arrow, t.body, text, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
        } else if (!info.icon.empty() && t.icon) {
            // Icon and label centered together.
            HDC mdc = g.GetHDC();
            int lw = textWidth(mdc, t.button, label), iw = t.s(26);
            g.ReleaseHDC(mdc);
            int left = std::max<int>(tr.left, (width(r) - lw - iw) / 2);
            iconAt(RECT{left, tr.top, left + iw, tr.bottom});
            RECT lr{left + iw, tr.top, tr.right, tr.bottom};
            drawText(g, label, lr, t.button, text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        } else {
            drawText(g, label, tr, t.button, text, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        }
        if (info.busy && info.progress) {
            double done;
            {
                std::lock_guard<std::mutex> lock(installMu_);
                done = modFraction_;
            }
            RectF track(t.sf(8), static_cast<REAL>(height(r)) - t.sf(5), static_cast<REAL>(width(r)) - t.sf(16), t.sf(2));
            fillRound(g, track, t.sf(1), mix(fill, text, 0.25f));
            RectF bar(track.X, track.Y, std::max(t.sf(2), static_cast<REAL>(track.Width * std::clamp(done, 0.0, 1.0))), track.Height);
            fillRound(g, bar, t.sf(1), text);
        }
        if (info.busy) {
            // Work in progress: a short light runs around the border (FlutterFX
            // Border Beam), once every 1.4 s.
            REAL bw = static_cast<REAL>(width(r)) - 1, bh = static_cast<REAL>(height(r)) - 1, rad = t.sf(6);
            REAL pen = t.sf(2);
            REAL perimeter = 2 * (bw + bh) - (8 - 2 * 3.14159265f) * rad;
            float phase = static_cast<float>(GetTickCount64() % 1400) / 1400.0f;
            REAL seg = perimeter / 4;
            Pen beam(gc(t.c.online), pen);
            REAL pattern[] = {seg / pen, (perimeter - seg) / pen};
            beam.SetDashPattern(pattern, 2);
            beam.SetDashOffset(-phase * perimeter / pen);
            GraphicsPath path;
            roundRectPath(path, RectF(0.5f + pen / 2, 0.5f + pen / 2, bw - pen, bh - pen), rad);
            g.DrawPath(&beam, &path);
        }
        if ((state & CDIS_FOCUS) && !(SendMessageW(button, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
            RectF f(t.sf(2), t.sf(2), static_cast<REAL>(width(r)) - t.sf(4), static_cast<REAL>(height(r)) - t.sf(4));
            strokeRound(g, f, t.sf(5), t.c.focus, t.sf(1.5));
        }
        }
        }
    }
    BitBlt(hdc, r.left, r.top, width(r), height(r), buf.dc, 0, 0, SRCCOPY);
}

void Launcher::drawNavItem(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1) || d.itemID >= navItems_.size()) return;
    const Theme& t = theme_;
    const NavItem& n = navItems_[d.itemID];
    RECT r = d.rcItem;
    Buffer buf(d.hDC, width(r), height(r));
    {
        Graphics g(buf.dc);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        SolidBrush bg(gc(t.c.sidebar));
        g.SetSmoothingMode(SmoothingModeNone);  // see drawButton
        g.FillRectangle(&bg, 0, 0, width(r), height(r));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        bool sel = (d.itemState & ODS_SELECTED) != 0;
        // The views (all, favorites, ready, rooms) and each group of categories are separated by a line.
        if (d.itemID > 0 && navItems_[d.itemID - 1].group != n.group) {
            Pen divider(gc(mix(t.c.sidebar, t.c.sidebarTextMuted, 0.28f)), 1);
            g.DrawLine(&divider, static_cast<REAL>(t.s(14)), 0.0f, static_cast<REAL>(width(r) - t.s(14)), 0.0f);
        }
        RectF box(0, t.sf(2), static_cast<REAL>(width(r)), static_cast<REAL>(height(r)) - t.sf(4));
        if (sel) fillRound(g, box, t.sf(6), mix(t.c.sidebar, t.c.sidebarText, 0.10f));
        Color labelColor = sel ? t.c.sidebarText : mix(t.c.sidebarText, t.c.sidebarTextMuted, 0.4f);
        int left = t.s(14);
        if (!n.icon.empty() && t.icon) {
            RECT ir{left, 0, left + t.s(22), height(r)};
            drawText(g, n.icon, ir, t.icon, sel ? t.c.sidebarText : t.c.sidebarTextMuted, DT_SINGLELINE | DT_VCENTER);
            left += t.s(30);
        }
        RECT tr{left, 0, width(r) - t.s(48), height(r)};
        drawText(g, n.label, tr, sel ? t.strong : t.body, labelColor, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        RECT cr{width(r) - t.s(48), 0, width(r) - t.s(12), height(r)};
        if (n.key == "rooms") {
            // Players online right now, live, in P2's color.
            if (onlineOn_ && roomsKnown_ && n.count > 0) {
                const auto& pc = cfg_.branding.playerColors;
                Color live = pc.size() > 1 ? pc[1] : t.c.online;
                HDC mdc = g.GetHDC();
                int cw = textWidth(mdc, t.caption, std::to_wstring(n.count));
                g.ReleaseHDC(mdc);
                SolidBrush dot(gc(live));
                g.FillEllipse(&dot, static_cast<REAL>(cr.right - cw - t.s(12)), static_cast<REAL>(height(r) / 2 - t.s(3)),
                              t.sf(6), t.sf(6));
                drawText(g, std::to_wstring(n.count), cr, t.caption, live, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            }
        } else {
            drawText(g, std::to_wstring(n.count), cr, t.caption, t.c.sidebarTextMuted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        if ((d.itemState & ODS_FOCUS) && GetFocus() == nav_ &&
            !(SendMessageW(hwnd_, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            strokeRound(g, box, t.sf(6), t.c.focus, t.sf(1.5));
    }
    BitBlt(d.hDC, r.left, r.top, width(r), height(r), buf.dc, 0, 0, SRCCOPY);
}

std::wstring Launcher::categoryText(const GameSpec& game) const {
    std::wstring out;
    if (displayName(game) != game.name) out = game.name;  // a mod: its own name under the game's
    for (const auto& c : game.categories)
        if (auto cat = cfg_.findCategory(c)) out += (out.empty() ? L"" : L", ") + cat->name;
    return out;
}

std::wstring Launcher::statsText(const GameStats* st) const {
    if (!st || st->plays <= 0) return cfg_.t("detail.never");
    if (st->plays == 1)
        return cfg_.t("detail.statsOnce", {{L"TIME", formatDuration(st->seconds)}, {L"WHEN", formatAgo(st->lastPlayed)}});
    return cfg_.t("detail.stats", {{L"COUNT", std::to_wstring(st->plays)},
                                   {L"TIME", formatDuration(st->seconds)},
                                   {L"WHEN", formatAgo(st->lastPlayed)}});
}

// The player slots: one tile per player the game supports, topped with that
// player's color. While hosting, each tile names who is in that slot (the host
// is P1 and friends follow in the order they joined, as the online mods do).
void Launcher::paintSlots(Graphics& g, const GameSpec& game) {
    const Theme& t = theme_;
    const auto& colors = cfg_.branding.playerColors;
    int n = game.maxPlayers;
    if (n <= 0 || colors.empty()) return;
    bool hosting = relay_.running() && relayGame_ == selected_;
    std::vector<std::wstring> names;
    if (hosting) {
        RelaySnapshot snap = relay_.snapshot();
        for (const auto& p : snap.peers)
            names.push_back(p.host ? cfg_.t("slot.host", {{L"NAME", p.name}}) : p.name);
    }
    int gap = t.s(8), w = width(dl_.slots);
    int tileW = (w - gap * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        RECT r{dl_.slots.left + i * (tileW + gap), dl_.slots.top, dl_.slots.left + i * (tileW + gap) + tileW,
               dl_.slots.bottom};
        bool filled = hosting && i < static_cast<int>(names.size());
        if (auto j = joinFrom_.find(i); filled && j != joinFrom_.end())
            if (ULONGLONG e = since(j->second); e < kJoinMs) {
                // Arrives from 90% size, overshoots a little, settles at 100%.
                float grow = 0.1f * motion::singleBounce(static_cast<float>(e) / kJoinMs) - 0.1f;
                InflateRect(&r, static_cast<int>(width(r) * grow / 2), static_cast<int>(height(r) * grow / 2));
            }
        fillRound(g, rf(r), t.sf(6), t.c.surfaceHover);
        RectF top(rf(r).X, rf(r).Y, rf(r).Width, t.sf(5));
        Region old;
        g.GetClip(&old);
        GraphicsPath shape;
        roundRectPath(shape, rf(r), t.sf(6));
        g.SetClip(&shape, CombineModeIntersect);
        SolidBrush colorBar(gc(colors[i % colors.size()]));
        g.FillRectangle(&colorBar, top);
        g.SetClip(&old);
        std::wstring label = cfg_.t("slot.label", {{L"N", std::to_wstring(i + 1)}});
        if (!hosting) {
            RECT lr{r.left, r.top + t.s(5), r.right, r.bottom};
            drawText(g, label, lr, t.button, t.c.text, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            continue;
        }
        if (!filled) {
            // An open slot waits in its player's color. The breath travels along
            // the row; without animations it holds halfway.
            float a = motion_ ? 0.2f + 0.6f * motion::wave(GetTickCount64(), i) : 0.5f;
            strokeRound(g, RectF(rf(r).X + 0.75f, rf(r).Y + 0.75f, rf(r).Width - 1.5f, rf(r).Height - 1.5f), t.sf(6),
                        mix(t.c.border, colors[i % colors.size()], a), t.sf(1.5));
        }
        RECT lr{r.left + t.s(10), r.top + t.s(9), r.right - t.s(6), r.top + t.s(29)};
        drawText(g, label, lr, t.button, t.c.text, DT_SINGLELINE | DT_VCENTER);
        RECT nr{r.left + t.s(10), r.top + t.s(29), r.right - t.s(6), r.bottom - t.s(6)};
        drawText(g, filled ? names[i] : cfg_.t("slot.open"), nr, t.caption, filled ? t.c.text : t.c.textMuted,
                 DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    }
}

}  // namespace fl::ui
