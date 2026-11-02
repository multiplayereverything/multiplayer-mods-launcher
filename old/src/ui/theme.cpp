#include "theme.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <wincodec.h>

#include "core/log.h"
#include "core/strings.h"

namespace fl::ui {

using namespace Gdiplus;

Gdiplus::Color gc(const fl::Color& c, uint8_t alpha) {
    return Gdiplus::Color(static_cast<BYTE>(c.a * alpha / 255), c.r, c.g, c.b);
}

fl::Color mix(const fl::Color& a, const fl::Color& b, float t) {
    auto m = [t](uint8_t x, uint8_t y) { return static_cast<uint8_t>(x + (y - x) * t + 0.5f); };
    return {m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), m(a.a, b.a)};
}

float luminance(const fl::Color& c) { return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f; }

void roundRectPath(GraphicsPath& path, const RectF& r, float radius) {
    float d = std::min({radius * 2, r.Width, r.Height});
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

void fillRound(Graphics& g, const RectF& r, float radius, const fl::Color& c) {
    GraphicsPath p;
    roundRectPath(p, r, radius);
    SolidBrush b(gc(c));
    g.FillPath(&b, &p);
}

void strokeRound(Graphics& g, const RectF& r, float radius, const fl::Color& c, float width) {
    GraphicsPath p;
    RectF in(r.X + width / 2, r.Y + width / 2, r.Width - width, r.Height - width);
    roundRectPath(p, in, radius);
    Pen pen(gc(c), width);
    g.DrawPath(&pen, &p);
}

void drawText(Graphics& g, const std::wstring& text, RECT r, HFONT font, const fl::Color& c, UINT flags) {
    HDC dc = g.GetHDC();
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c.ref());
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
    g.ReleaseHDC(dc);
}

int textHeight(HDC dc, HFONT font, const std::wstring& text, int width, UINT flags) {
    if (text.empty()) return 0;
    RECT r{0, 0, std::max(width, 1), 0};
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

int textWidth(HDC dc, HFONT font, const std::wstring& text) {
    RECT r{0, 0, 0, 0};
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.right - r.left;
}

std::wstring fitText(HDC dc, HFONT font, const std::wstring& text, int width, int maxHeight) {
    if (textHeight(dc, font, text, width) <= maxHeight) return text;
    if (textHeight(dc, font, L"Ag", width) > maxHeight) return {};
    // Binary search over word boundaries for the longest prefix that fits with the ellipsis.
    std::vector<size_t> cuts;
    for (size_t i = 1; i < text.size(); ++i)
        if (text[i] == L' ' && text[i - 1] != L' ') cuts.push_back(i);
    size_t lo = 0, hi = cuts.size();
    std::wstring best;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        std::wstring candidate = text.substr(0, cuts[mid]);
        while (!candidate.empty() && (candidate.back() == L',' || candidate.back() == L'.' || candidate.back() == L':'))
            candidate.pop_back();
        candidate += L"\u2026";
        if (textHeight(dc, font, candidate, width) <= maxHeight) {
            best = candidate;
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best;
}

// GDI+ has no WebP decoder, but Windows' imaging component does when the WebP extension is installed
// (it is on current Windows 10 and 11). No codec just means the cover falls back to the initials art.
static std::unique_ptr<Bitmap> loadWebp(const std::wstring& path) {
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninit = SUCCEEDED(init);  // RPC_E_CHANGED_MODE: COM is already up in another mode, use it as is
    std::unique_ptr<Bitmap> out;
    {
        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                         WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
            w <= 8192 && h <= 8192 && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            auto bmp = std::make_unique<Bitmap>(static_cast<INT>(w), static_cast<INT>(h), PixelFormat32bppPARGB);
            BitmapData data{};
            Rect all(0, 0, static_cast<INT>(w), static_cast<INT>(h));
            if (bmp->LockBits(&all, ImageLockModeWrite, PixelFormat32bppPARGB, &data) == Ok) {
                bool upright = data.Stride > 0;
                HRESULT hr = upright ? conv->CopyPixels(nullptr, static_cast<UINT>(data.Stride),
                                                        static_cast<UINT>(data.Stride) * h,
                                                        static_cast<BYTE*>(data.Scan0))
                                     : E_FAIL;
                bmp->UnlockBits(&data);
                if (SUCCEEDED(hr)) out = std::move(bmp);
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
        if (factory) factory->Release();
    }
    if (uninit) CoUninitialize();
    return out;
}

std::unique_ptr<Bitmap> loadImageFile(const std::wstring& path) {
    if (path.empty()) return nullptr;
    // A picture that isn't there is not a bad picture: a website cover is downloaded after the first paint,
    // and the cards look again when it arrives.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logger().debug(L"Picture not there (yet): " + path);
        return nullptr;
    }
    if (path.size() > 5 && toLower(path.substr(path.size() - 5)) == L".webp") {
        auto webp = loadWebp(path);
        if (!webp) logger().warn(L"Could not load " + path + L" (Windows needs its WebP image extension)");
        return webp;
    }
    std::unique_ptr<Bitmap> decoded(Bitmap::FromFile(path.c_str()));
    if (!decoded || decoded->GetLastStatus() != Ok || decoded->GetWidth() == 0) {
        logger().warn(L"Could not load image " + path + L" (use PNG, JPG or BMP)");
        return nullptr;
    }
    // Copy into a premultiplied bitmap: faster to draw, and the file is no longer locked.
    auto copy = std::make_unique<Bitmap>(decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppPARGB);
    Graphics g(copy.get());
    g.DrawImage(decoded.get(), 0, 0, decoded->GetWidth(), decoded->GetHeight());
    return copy;
}

std::unique_ptr<Bitmap> loadImageBytes(const std::string& bytes) {
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!mem) return nullptr;
    memcpy(GlobalLock(mem), bytes.data(), bytes.size());
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream))) {
        GlobalFree(mem);
        return nullptr;
    }
    std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream));
    std::unique_ptr<Bitmap> copy;
    if (decoded && decoded->GetLastStatus() == Ok && decoded->GetWidth() > 0) {
        // Copied out while the stream is alive: GDI+ reads it lazily.
        copy = std::make_unique<Bitmap>(decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppPARGB);
        Graphics g(copy.get());
        g.DrawImage(decoded.get(), 0, 0, decoded->GetWidth(), decoded->GetHeight());
    }
    decoded.reset();
    stream->Release();
    return copy;
}

static std::wstring initialsOf(const std::wstring& name) {
    std::wstring out;
    for (const auto& word : split(name, L' ')) {
        if (word.empty()) continue;
        if (iswdigit(word[0])) out += word;  // "Gun Mayhem 2" -> "GM2"
        else if (iswalnum(word[0])) out += static_cast<wchar_t>(towupper(word[0]));
        if (out.size() >= 3) break;
    }
    return out.empty() ? L"?" : out.substr(0, 3);
}

// A 5x7 pixel font for cover initials, drawn by hand for this launcher:
// A-Z then 0-9, one byte per row, bit 4 is the leftmost column.
static const uint8_t kPixelFont[36][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // A
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},  // B
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},  // C
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},  // D
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F},  // E
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},  // F
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F},  // G
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},  // H
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},  // I
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},  // J
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},  // K
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},  // L
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11},  // M
    {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},  // N
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // O
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},  // P
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D},  // Q
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},  // R
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E},  // S
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},  // T
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},  // U
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},  // V
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A},  // W
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},  // X
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04},  // Y
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},  // Z
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E},  // 0
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},  // 1
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},  // 2
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},  // 3
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02},  // 4
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},  // 5
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E},  // 6
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},  // 7
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},  // 8
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},  // 9
};

static const uint8_t* pixelGlyph(wchar_t ch) {
    ch = static_cast<wchar_t>(towupper(ch));
    if (ch >= L'A' && ch <= L'Z') return kPixelFont[ch - L'A'];
    if (ch >= L'0' && ch <= L'9') return kPixelFont[26 + ch - L'0'];
    return nullptr;
}

std::unique_ptr<Bitmap> coverArt(const std::wstring& name, const fl::Color& from, const fl::Color& to, int w, int h) {
    auto bmp = std::make_unique<Bitmap>(w, h, PixelFormat32bppPARGB);
    Graphics g(bmp.get());
    REAL W = static_cast<REAL>(w), H = static_cast<REAL>(h);
    SolidBrush base(gc(from));
    g.FillRectangle(&base, 0.0f, 0.0f, W, H);
    // A floor band in the second color: the ground line of a platform level.
    REAL floorY = std::floor(H * 0.78f);
    SolidBrush floor(gc(to));
    g.FillRectangle(&floor, 0.0f, floorY, W, H - floorY + 1);

    std::vector<const uint8_t*> glyphs;
    for (wchar_t ch : initialsOf(name))
        if (const uint8_t* gl = pixelGlyph(ch)) glyphs.push_back(gl);
    if (glyphs.empty()) return bmp;
    // Whole-number block size so every block is identical: the pixel look.
    int cols = static_cast<int>(glyphs.size()) * 6 - 1, rowsN = 7;
    REAL px = std::floor(std::min(W * 0.6f / cols, floorY * 0.55f / rowsN));
    if (px < 1) px = 1;
    REAL x0 = std::floor((W - cols * px) / 2), y0 = std::floor((floorY - rowsN * px) / 2);
    SolidBrush ink(Gdiplus::Color(255, 255, 255, 255));
    for (size_t i = 0; i < glyphs.size(); ++i)
        for (int r = 0; r < 7; ++r)
            for (int c = 0; c < 5; ++c)
                if (glyphs[i][r] & (0x10 >> c)) g.FillRectangle(&ink, x0 + (i * 6 + c) * px, y0 + r * px, px, px);
    return bmp;
}

void drawCover(Graphics& g, Bitmap& img, const RectF& box, float radius, bool roundBottom, float focus) {
    float w = static_cast<float>(img.GetWidth()), h = static_cast<float>(img.GetHeight());
    float scale = std::max(box.Width / w, box.Height / h);
    RectF src((w - box.Width / scale) / 2, (h - box.Height / scale) * focus, box.Width / scale, box.Height / scale);
    GraphicsPath p;
    if (roundBottom) {
        roundRectPath(p, box, radius);
    } else {
        // Rounded top only: the card body continues below the art.
        float d = std::min({radius * 2, box.Width, box.Height});
        p.AddArc(box.X, box.Y, d, d, 180, 90);
        p.AddArc(box.X + box.Width - d, box.Y, d, d, 270, 90);
        p.AddLine(box.X + box.Width, box.Y + box.Height, box.X, box.Y + box.Height);
        p.CloseFigure();
    }
    Region old;
    g.GetClip(&old);
    g.SetClip(&p, CombineModeIntersect);
    g.DrawImage(&img, box, src.X, src.Y, src.Width, src.Height, UnitPixel);
    g.SetClip(&old);
}

// ---- Theme -----------------------------------------------------------------

static bool fontInstalled(const std::wstring& family) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, family.c_str(), _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

Theme::~Theme() {
    release();
    if (iconBig) DestroyIcon(iconBig);
    if (iconSmall) DestroyIcon(iconSmall);
}

void Theme::init(const Config& cfg) {
    cfg_ = &cfg;
    configured_ = cfg.branding.colors;
    family_ = cfg.branding.font;
    if (!fontInstalled(family_)) {
        logger().warn(L"Font '" + family_ + L"' is not available; using Segoe UI.");
        family_ = L"Segoe UI";
    }
    // Variable fonts such as Bahnschrift appear to GDI as one family per named
    // instance ("Bahnschrift SemiBold Condensed"), so look for those first.
    display_ = cfg.branding.displayFont;
    displayCondensed_ = display_ + L" SemiBold Condensed";
    if (fontInstalled(display_ + L" SemiBold")) display_ += L" SemiBold";
    else if (!fontInstalled(display_)) display_ = family_;
    if (!fontInstalled(displayCondensed_)) displayCondensed_ = display_;
    for (const wchar_t* face : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"})
        if (iconFace_.empty() && fontInstalled(face)) iconFace_ = face;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    iconBig = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                            GetSystemMetrics(SM_CYICON), 0));
    iconSmall = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), 0));
}

void Theme::release() {
    for (HFONT* f : {&body, &strong, &caption, &tiny, &title, &brand, &heading, &name, &button, &icon}) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
    for (HBRUSH* b : {&brBackground, &brSurface, &brSidebar}) {
        if (*b) DeleteObject(*b);
        *b = nullptr;
    }
}

void Theme::build(UINT newDpi) {
    dpi = newDpi ? newDpi : 96;
    release();
    HIGHCONTRASTW hc{sizeof hc};
    highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
    c = configured_;
    if (highContrast) {
        auto sys = [](int idx) {
            COLORREF v = GetSysColor(idx);
            return fl::Color{GetRValue(v), GetGValue(v), GetBValue(v), 255};
        };
        c.background = c.surface = c.surfaceHover = c.sidebar = sys(COLOR_WINDOW);
        c.text = c.sidebarText = c.border = c.success = c.warning = c.error = c.online = sys(COLOR_WINDOWTEXT);
        c.textMuted = c.sidebarTextMuted = sys(COLOR_WINDOWTEXT);
        c.primary = c.primaryHover = c.focus = sys(COLOR_HIGHLIGHT);
        c.onPrimary = sys(COLOR_HIGHLIGHTTEXT);
    }
    dark = luminance(c.background) < 0.5f;
    float pt = cfg_ ? cfg_->branding.fontSize : 10.0f;
    auto makeFace = [&](const std::wstring& face, float size, int weight) {
        LOGFONTW lf{};
        lf.lfHeight = -static_cast<LONG>(size * dpi / 72.0f + 0.5f);
        lf.lfWeight = weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
        return CreateFontIndirectW(&lf);
    };
    auto make = [&](float size, int weight) { return makeFace(family_, size, weight); };
    body = make(pt, FW_NORMAL);
    strong = make(pt, FW_SEMIBOLD);
    caption = make(pt - 1.0f, FW_NORMAL);
    tiny = make(pt - 2.0f, FW_BOLD);
    heading = makeFace(display_, pt * 1.3f, FW_SEMIBOLD);
    title = makeFace(displayCondensed_, pt * 2.3f, FW_SEMIBOLD);
    brand = makeFace(display_, pt * 1.45f, FW_SEMIBOLD);
    name = makeFace(displayCondensed_, pt * 1.35f, FW_SEMIBOLD);
    button = makeFace(display_, pt * 1.25f, FW_SEMIBOLD);
    if (!iconFace_.empty()) icon = makeFace(iconFace_, pt * 1.2f, FW_NORMAL);
    brBackground = CreateSolidBrush(c.background.ref());
    brSurface = CreateSolidBrush(c.surface.ref());
    brSidebar = CreateSolidBrush(c.sidebar.ref());
}

}  // namespace fl::ui
