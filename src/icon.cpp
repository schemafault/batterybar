#include "icon.h"

#include "appearance.h"

#include <algorithm>
#include <string>

namespace {

int CALLBACK FoundFont(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;  // one match is enough
}

bool FontInstalled(HDC dc, const wchar_t* face) {
    LOGFONTW lf = {};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FoundFont, reinterpret_cast<LPARAM>(&found), 0);
    return found;
}

// The font holding the device pictures, or null if this Windows has neither.
const wchar_t* PictureFace(HDC dc) {
    static const wchar_t* face = FontInstalled(dc, L"Segoe Fluent Icons")  ? L"Segoe Fluent Icons"
                                 : FontInstalled(dc, L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                                           : nullptr;
    return face;
}

// A square 32-bit bitmap that GDI can draw on and we can read back.
struct Canvas {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    uint32_t* px = nullptr;
    int size = 0;

    explicit Canvas(int s) : size(s) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s;
        bi.bmiHeader.biHeight = -s;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        void* bits = nullptr;
        if (dc) bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap) {
            previous = SelectObject(dc, bitmap);
            px = static_cast<uint32_t*>(bits);
        }
    }
    ~Canvas() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    bool ok() const { return px != nullptr; }
    size_t count() const { return static_cast<size_t>(size) * static_cast<size_t>(size); }
};

// GDI text has no alpha channel. Drawing in white on black and reading the
// brightness back gives how much of each pixel the text covers (0..255).
std::vector<uint8_t> Coverage(Canvas& c, const wchar_t* face, int weight, int em, const std::wstring& text, UINT align,
                              int y) {
    std::vector<uint8_t> cov(c.count(), 0);
    HFONT font = CreateFontW(-em, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (!font) return cov;
    std::fill(c.px, c.px + c.count(), 0u);
    HGDIOBJ oldFont = SelectObject(c.dc, font);
    SetBkMode(c.dc, TRANSPARENT);
    SetTextColor(c.dc, RGB(255, 255, 255));
    SetTextAlign(c.dc, TA_CENTER | align);
    TextOutW(c.dc, c.size / 2, y, text.c_str(), static_cast<int>(text.size()));
    GdiFlush();
    SelectObject(c.dc, oldFont);
    DeleteObject(font);
    for (size_t i = 0; i < cov.size(); ++i) cov[i] = static_cast<uint8_t>((c.px[i] >> 8) & 0xFF);
    return cov;
}

// Moves the drawn shape to the middle of rows [0, bottom), whatever the font's
// own spacing is: a tall mouse and a flat keyboard both end up centred.
void CentreInBand(std::vector<uint8_t>& cov, int size, int bottom) {
    int x0 = size, x1 = -1, y0 = size, y1 = -1;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x)
            if (cov[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)]) {
                x0 = std::min(x0, x), x1 = std::max(x1, x);
                y0 = std::min(y0, y), y1 = std::max(y1, y);
            }
    if (x1 < 0) return;  // nothing drawn
    const int dx = (size - (x1 - x0 + 1)) / 2 - x0;
    const int dy = std::max(0, (bottom - (y1 - y0 + 1)) / 2) - y0;
    if (dx == 0 && dy == 0) return;
    std::vector<uint8_t> moved(cov.size(), 0);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
            moved[static_cast<size_t>(ny) * static_cast<size_t>(size) + static_cast<size_t>(nx)] =
                cov[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
        }
    cov.swap(moved);
}

void Paint(std::vector<uint32_t>& out, const std::vector<uint8_t>& cov, uint32_t colour, uint8_t opacity) {
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t alpha = static_cast<uint32_t>(cov[i]) * opacity / 255;
        if (alpha > (out[i] >> 24)) out[i] = (colour & 0x00FFFFFFu) | (alpha << 24);
    }
}

}  // namespace

std::vector<uint32_t> RenderIcon(const BatteryInfo& b, int size, bool lightTaskbar) {
    if (size <= 0) return {};
    Canvas canvas(size);
    if (!canvas.ok()) return {};

    const ButtonAppearance look = Describe(b, lightTaskbar);
    const IconLayout at = LayoutFor(size);
    std::vector<uint32_t> out(canvas.count(), 0u);

    // The picture: a Windows icon glyph, or the device's letter if the icon fonts are missing.
    const wchar_t* pictureFace = PictureFace(canvas.dc);
    std::vector<uint8_t> picture =
        pictureFace ? Coverage(canvas, pictureFace, FW_NORMAL, at.glyphEm, std::wstring(1, look.glyph), TA_TOP, 0)
                    : Coverage(canvas, L"Segoe UI", FW_SEMIBOLD, at.glyphEm, std::wstring(1, KindLetter(b.kind)), TA_TOP, 0);
    CentreInBand(picture, size, at.glyphBottom);
    Paint(out, picture, look.glyphColour, look.opacity);

    Paint(out, Coverage(canvas, L"Segoe UI", FW_SEMIBOLD, at.textEm, look.text, TA_BASELINE, at.textBaseline),
          look.textColour, look.opacity);
    return out;
}

HICON MakeIcon(const BatteryInfo& b, int size, bool lightTaskbar) {
    const std::vector<uint32_t> pixels = RenderIcon(b, size, lightTaskbar);
    if (pixels.empty()) return nullptr;
    Canvas canvas(size);
    if (!canvas.ok()) return nullptr;
    std::copy(pixels.begin(), pixels.end(), canvas.px);

    // The alpha channel does the masking; the mask bitmap only has to exist.
    std::vector<BYTE> zeros(static_cast<size_t>((size + 15) / 16) * 2 * static_cast<size_t>(size), 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zeros.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = canvas.bitmap;
    HICON icon = CreateIconIndirect(&ii);
    if (mask) DeleteObject(mask);
    return icon;
}
