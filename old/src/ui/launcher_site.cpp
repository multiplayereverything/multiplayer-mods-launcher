// What the website says about each game (core/site.h): fetched in the background,
// shown as a grade chip on the card and an info block in the details panel.
#include <algorithm>

#include "core/fs.h"
#include "core/log.h"
#include "core/process.h"
#include "core/strings.h"
#include "launcher.h"
#include "launcher_internal.h"
// win.h (via theme.h) first: these need the full Windows types.
#include <commctrl.h>

namespace fl::ui {

using namespace Gdiplus;
using namespace detail;

namespace {

constexpr const char* kSyncedKeys[] = {"see", "story", "enemies", "world", "items"};

}  // namespace

const SiteGame* Launcher::siteOf(int gi) const {
    if (gi < 0 || gi >= static_cast<int>(cfg_.games.size())) return nullptr;
    const GameSpec& g = cfg_.games[gi];
    return findSite(site_, g.siteId, g.id, g.onlineId);
}

void Launcher::startSiteFetch() {
    if (cfg_.downloads.site.empty() || getenv("FL_NO_SITE")) return;
    std::wstring url = cfg_.downloads.site, cache = fs::join(lib_.dataDir(), L"site.json"), dataDir = lib_.dataDir();
    HWND wnd = hwnd_;
    siteThread_ = std::thread([this, url, cache, wnd, dataDir] {
        std::vector<SiteGame> list;
        std::wstring err;
        if (!loadSite(url, cache, list, err)) logger().info(L"Website: " + err);
        int covers = fetchSiteCovers(cfg_, list, url, fs::join(dataDir, L"covers"));
        {
            std::lock_guard<std::mutex> lock(installMu_);
            pendingSite_ = std::move(list);
        }
        PostMessageW(wnd, WM_APP_SITE, static_cast<WPARAM>(covers), 0);
    });
}

void Launcher::onSite(int newCovers) {
    if (newCovers > 0) {  // a picture that wasn't there before: let the cards look for it again
        images_.clear();
        imageTried_.clear();
        artCache_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(installMu_);
        site_ = std::move(pendingSite_);
        pendingSite_.clear();
    }
    if (guidePage()) {  // the page uses the website's facts: build it again with them
        guide_ = buildGuide(cfg_, cfg_.games[guideGame_], siteOf(guideGame_));
        layoutGuidePage();
    }
    InvalidateRect(grid_, nullptr, FALSE);
    layoutDetail();
    InvalidateRect(hwnd_, &detail_, FALSE);
}

// ---- the grade chip ----------------------------------------------------------------

namespace {

// Dark text on each: 7:1 or better.
Color gradeFill(const std::string& q) {
    if (q == "A") return {0x3C, 0xCB, 0x7F, 255};
    if (q == "B") return {0x8F, 0xB0, 0xFF, 255};
    if (q == "C") return {0xF2, 0xB5, 0x44, 255};
    if (q == "D") return {0xFF, 0x8A, 0x5C, 255};
    return {0x9A, 0xA1, 0xAC, 255};
}

}  // namespace

// One capital letter or sign in the middle of `box`. Centering the font's whole line box leaves a capital sitting
// high or low (the display font has a tall ascent), so the baseline is placed from the font's real cap height.
void Launcher::drawGlyphCentered(Graphics& g, const std::wstring& glyph, const RECT& box, const Color& color) {
    const Theme& t = theme_;
    HDC dc = g.GetHDC();
    HGDIOBJ old = SelectObject(dc, t.button);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    GLYPHMETRICS gm{};
    MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
    int capH = tm.tmAscent - tm.tmInternalLeading;  // fallback: the em height above the baseline
    if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR && gm.gmBlackBoxY > 0)
        capH = static_cast<int>(gm.gmBlackBoxY);
    int ascent = tm.tmAscent;
    SelectObject(dc, old);
    g.ReleaseHDC(dc);
    // The baseline goes capH/2 below the middle; the text's top is `ascent` above the baseline.
    int top = (box.top + box.bottom) / 2 + capH / 2 - ascent;
    // DT_CENTER centers the advance width, which leaves the ink a pixel to the left of the middle.
    RECT r{box.left + t.s(1), top, box.right + t.s(1), top + tm.tmHeight};
    drawText(g, glyph, r, t.button, color, DT_SINGLELINE | DT_CENTER | DT_TOP | DT_NOCLIP);
}

// A rounded square with the grade letter; `box` is its place.
void Launcher::paintGradeChip(Graphics& g, const RECT& box, const std::string& grade) {
    const Theme& t = theme_;
    fillRound(g, rf(box), t.sf(5), gradeFill(grade));
    drawGlyphCentered(g, widen(grade), box, Color{0x15, 0x17, 0x1C, 255});
}

// ---- the details panel's block -------------------------------------------------------

namespace {

struct Flow {
    int x, y, right, lineH, gap;
};

}  // namespace

// Height of the block for the selected game at width `w`, 0 when the website has nothing to say.
int Launcher::siteBlockHeight(HDC dc, int w, const SiteGame* s, bool soon) {
    if (!s || (s->status.empty() && s->quality.empty())) return 0;
    const Theme& t = theme_;
    int h = 0;
    // Grade row: the chip and its meaning (wraps).
    std::wstring note = soon ? cfg_.t("site.planned")
                        : !s->quality.empty() && cfg_.text.count("site.quality." + s->quality) ? cfg_.t("site.quality." + s->quality)
                                                                                                : cfg_.t("site.unrated");
    int chip = t.s(24);
    h += std::max(chip, textHeight(dc, t.caption, note, w - chip - t.s(10))) + t.s(8);
    h += textHeight(dc, t.caption, siteFacts(*s, soon), w) + t.s(8);
    int dots = siteDotsHeight(dc, w, *s);
    if (dots) h += dots + t.s(4);
    return h;
}

std::wstring Launcher::siteFacts(const SiteGame& s, bool soon) const {
    std::wstring out;
    auto add = [&](const std::wstring& part) {
        if (!part.empty()) out += (out.empty() ? L"" : L", ") + part;
    };
    std::string key = "site.status." + (soon ? std::string("coming-soon") : s.status);
    if (cfg_.text.count(key)) add(cfg_.t(key));
    if (!s.version.empty() && !soon)
        add(cfg_.t(s.status == "testing" ? "site.testBuild" : "site.version", {{L"VERSION", s.version}}));
    if (!s.updated.empty() && !soon) add(cfg_.t("site.updated", {{L"DATE", s.updated}}));
    if (!s.connect.empty()) add(s.connect);
    return out;
}

// The "in sync" row needs a line of its own only when the website knows something about it.
static bool anySynced(const SiteGame& s) {
    for (const char* k : kSyncedKeys)
        if (auto it = s.synced.find(k); it != s.synced.end() && it->second != "unknown") return true;
    return false;
}

int Launcher::siteDotsHeight(HDC dc, int w, const SiteGame& s) {
    if (!anySynced(s)) return 0;
    const Theme& t = theme_;
    int lineH = t.s(20), x = 0, lines = 1;
    int headW = textWidth(dc, t.caption, cfg_.t("site.synced")) + t.s(10);
    x = headW;
    for (const char* k : kSyncedKeys) {
        int iw = t.s(16) + textWidth(dc, t.caption, cfg_.t(std::string("site.synced.") + k)) + t.s(12);
        if (x + iw > w) {
            ++lines;
            x = 0;
        }
        x += iw;
    }
    return lines * lineH;
}

void Launcher::paintSite(Graphics& g, HDC dc, const RECT& r, const SiteGame& s, bool soon) {
    const Theme& t = theme_;
    int w = width(r), y = r.top;
    // Grade chip and what it means.
    std::wstring note = soon ? cfg_.t("site.planned")
                        : !s.quality.empty() && cfg_.text.count("site.quality." + s.quality) ? cfg_.t("site.quality." + s.quality)
                                                                                              : cfg_.t("site.unrated");
    int chip = t.s(24);
    bool graded = !s.quality.empty() && cfg_.text.count("site.quality." + s.quality);
    int noteH = textHeight(dc, t.caption, note, w - chip - t.s(10));
    int rowH = std::max(chip, noteH);
    if (graded) {
        paintGradeChip(g, RECT{r.left, y + (rowH - chip) / 2, r.left + chip, y + (rowH - chip) / 2 + chip}, s.quality);
    } else {
        strokeRound(g, rf(RECT{r.left, y + (rowH - chip) / 2, r.left + chip, y + (rowH - chip) / 2 + chip}), t.sf(5),
                    t.c.textMuted, t.sf(1.2));
        drawGlyphCentered(g, L"?", RECT{r.left, y + (rowH - chip) / 2, r.left + chip, y + (rowH - chip) / 2 + chip},
                          t.c.textMuted);
    }
    drawText(g, note, RECT{r.left + chip + t.s(10), y, r.right, y + rowH}, t.caption, t.c.text, DT_WORDBREAK | DT_VCENTER);
    y += rowH + t.s(8);
    std::wstring facts = siteFacts(s, soon);
    int factsH = textHeight(dc, t.caption, facts, w);
    drawText(g, facts, RECT{r.left, y, r.right, y + factsH}, t.caption, t.c.textMuted, DT_WORDBREAK);
    y += factsH + t.s(8);

    if (!anySynced(s)) return;
    // What stays in sync: a filled dot, a half dot or an empty ring, each with its word, so color is never alone.
    int lineH = t.s(20), x = r.left;
    std::wstring head = cfg_.t("site.synced");
    int headW = textWidth(dc, t.caption, head) + t.s(10);
    drawText(g, head, RECT{x, y, x + headW, y + lineH}, t.caption, t.c.textMuted, DT_SINGLELINE | DT_VCENTER);
    x += headW;
    for (const char* k : kSyncedKeys) {
        std::wstring label = cfg_.t(std::string("site.synced.") + k);
        int lw = textWidth(dc, t.caption, label);
        int iw = t.s(16) + lw + t.s(12);
        if (x + iw > r.right) {
            x = r.left;
            y += lineH;
        }
        std::string state = s.synced.count(k) ? s.synced.at(k) : "unknown";
        Color c = state == "synced" ? t.c.success : state == "partly" ? t.c.warning : t.c.textMuted;
        RectF dot(static_cast<REAL>(x), static_cast<REAL>(y + lineH / 2 - t.s(5)), t.sf(10), t.sf(10));
        if (state == "synced") {
            SolidBrush b(gc(c));
            g.FillEllipse(&b, dot);
        } else {
            Pen p(gc(c), t.sf(1.4));
            g.DrawEllipse(&p, dot);
            if (state == "partly") {
                SolidBrush b(gc(c));
                g.FillPie(&b, dot, 90.0f, 180.0f);  // the left half is filled
            }
        }
        drawText(g, label, RECT{x + t.s(16), y, x + t.s(16) + lw + t.s(4), y + lineH}, t.caption,
                 state == "unknown" ? t.c.textMuted : t.c.text, DT_SINGLELINE | DT_VCENTER);
        x += iw;
    }
}

// What an empty list says, by the view it is in.
std::wstring Launcher::emptyTitle() const {
    std::string key = navItems_.empty() ? "all" : navItems_[navIndex_].key;
    if (!trim(windowText(search_)).empty()) return cfg_.t("empty.search.title");
    if (key == "fav") return cfg_.t("empty.fav.title");
    if (key == "ready") return cfg_.t("empty.ready.title");
    if (key == "soon") return cfg_.t("empty.soon.title");
    return cfg_.t("empty.title");
}

std::wstring Launcher::emptyBody() const {
    std::string key = navItems_.empty() ? "all" : navItems_[navIndex_].key;
    if (!trim(windowText(search_)).empty()) return cfg_.t("empty.search.body");
    if (key == "fav") return cfg_.t("empty.fav.body");
    if (key == "ready") return cfg_.t("empty.ready.body");
    if (key == "soon") return cfg_.t("empty.soon.body");
    return cfg_.t("empty.body");
}

}  // namespace fl::ui
