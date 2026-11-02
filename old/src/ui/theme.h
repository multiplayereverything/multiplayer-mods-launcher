// Visual resources derived from branding: palette, DPI-scaled fonts, brushes,
// cached game art, plus small drawing helpers shared by the launcher window.
#pragma once
#include <map>
#include <memory>
#include <string>

#include "core/config.h"
#include "gdi.h"

namespace fl::ui {

struct Theme {
    ThemeColors c;
    bool highContrast = false;
    bool dark = true;
    UINT dpi = 96;

    // Body text in the UI font; title, heading, brand, name (card titles) and
    // button (action labels) in the display font.
    HFONT body = nullptr, strong = nullptr, caption = nullptr, tiny = nullptr, title = nullptr, brand = nullptr,
          heading = nullptr, name = nullptr, button = nullptr;
    // Segoe Fluent Icons (Windows 11) or Segoe MDL2 Assets (Windows 10); null when neither exists.
    HFONT icon = nullptr;
    HBRUSH brBackground = nullptr, brSurface = nullptr, brSidebar = nullptr;
    HICON iconBig = nullptr, iconSmall = nullptr;

    Theme() = default;
    Theme(const Theme&) = delete;
    Theme& operator=(const Theme&) = delete;
    ~Theme();

    void init(const Config& cfg);
    // (Re)creates DPI-dependent fonts and brushes; also re-reads high-contrast mode.
    void build(UINT newDpi);
    int s(double dp) const { return static_cast<int>(dp * dpi / 96.0 + 0.5); }
    float sf(double dp) const { return static_cast<float>(dp * dpi / 96.0); }

private:
    void release();
    const Config* cfg_ = nullptr;
    ThemeColors configured_;
    std::wstring family_, display_, displayCondensed_, iconFace_;
};

Gdiplus::Color gc(const Color& c, uint8_t alpha = 255);
Color mix(const Color& a, const Color& b, float t);  // t=0 -> a, t=1 -> b
float luminance(const Color& c);

void roundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius);
void fillRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Color& c);
void strokeRound(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius, const Color& c, float width);
// GDI text (ClearType) drawn through a Graphics object's HDC.
void drawText(Gdiplus::Graphics& g, const std::wstring& text, RECT r, HFONT font, const Color& c, UINT flags);
int textHeight(HDC dc, HFONT font, const std::wstring& text, int width, UINT flags = DT_WORDBREAK);
int textWidth(HDC dc, HFONT font, const std::wstring& text);
// `text` wrapped to `width`, shortened at a word with a trailing ellipsis until
// it fits in `maxHeight`. Empty when not even one line fits.
std::wstring fitText(HDC dc, HFONT font, const std::wstring& text, int width, int maxHeight);

std::unique_ptr<Gdiplus::Bitmap> loadImageFile(const std::wstring& path);
// The same from bytes in memory (a cover built into the exe).
std::unique_ptr<Gdiplus::Bitmap> loadImageBytes(const std::string& bytes);
// Generated cover art: the game's initials as pixel blocks on its own color.
std::unique_ptr<Gdiplus::Bitmap> coverArt(const std::wstring& name, const Color& from, const Color& to, int w, int h);
// Draws `img` scaled to cover `box` (cropping the overflow), clipped to rounded corners.
// `focus`: which part stays in view when the height is cropped (0 top, 1 bottom).
void drawCover(Gdiplus::Graphics& g, Gdiplus::Bitmap& img, const Gdiplus::RectF& box, float radius, bool roundBottom,
               float focus = 0.5f);

}  // namespace fl::ui
