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

COLORREF ToColorRef(uint32_t rgb) { return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

// `amount` of 255 gives the colour itself, 0 gives the background.
uint32_t Mix(uint32_t colour, uint32_t back, uint32_t amount) {
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8)
        out |= ((((colour >> shift) & 0xFF) * amount + ((back >> shift) & 0xFF) * (255 - amount)) / 255) << shift;
    return out;
}

HFONT UiFont(const wchar_t* face, int em, int weight) {
    return CreateFontW(-em, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

int TextWidth(HDC dc, HFONT font, const std::wstring& text) {
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

void DrawIn(HDC dc, HFONT font, const std::wstring& text, RECT rc, uint32_t colour, UINT format) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, ToColorRef(colour));
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rc, format | DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
    SelectObject(dc, old);
}

void Pill(HDC dc, int left, int top, int right, int bottom, uint32_t colour) {
    HBRUSH brush = CreateSolidBrush(ToColorRef(colour));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, left, top, right + 1, bottom + 1, bottom - top, bottom - top);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void Paint(std::vector<uint32_t>& out, const std::vector<uint8_t>& cov, uint32_t colour, uint8_t opacity) {
    for (size_t i = 0; i < out.size(); ++i) {
        const uint32_t alpha = static_cast<uint32_t>(cov[i]) * opacity / 255;
        if (alpha > (out[i] >> 24)) out[i] = (colour & 0x00FFFFFFu) | (alpha << 24);
    }
}

}  // namespace

std::vector<uint32_t> RenderIcon(const BatteryInfo& b, int size, bool lightTaskbar, IconStyle style) {
    if (size <= 0) return {};
    Canvas canvas(size);
    if (!canvas.ok()) return {};

    const ButtonAppearance look = Describe(b, lightTaskbar);
    const IconLayout at = LayoutFor(size, style, static_cast<int>(look.text.size()));
    std::vector<uint32_t> out(canvas.count(), 0u);

    if (at.ring) {  // a faint full circle, with the battery level drawn over it
        const RingMask ring = RenderRing(size, look.level);
        Paint(out, ring.track, look.glyphColour, static_cast<uint8_t>(look.opacity / 5));
        Paint(out, ring.level, look.textColour, look.opacity);
    }

    if (at.glyphEm > 0) {  // a Windows icon glyph, or the device's letter if the icon fonts are missing
        const wchar_t* pictureFace = PictureFace(canvas.dc);
        std::vector<uint8_t> picture =
            pictureFace ? Coverage(canvas, pictureFace, FW_NORMAL, at.glyphEm, std::wstring(1, look.glyph), TA_TOP, 0)
                        : Coverage(canvas, L"Segoe UI", FW_SEMIBOLD, at.glyphEm, std::wstring(1, KindLetter(b.kind)), TA_TOP, 0);
        CentreInBand(picture, size, at.glyphBottom);
        Paint(out, picture, look.glyphColour, look.opacity);
    }

    if (at.textEm > 0) {
        const bool centred = at.textBaseline < 0;
        std::vector<uint8_t> number = Coverage(canvas, L"Segoe UI", FW_SEMIBOLD, at.textEm, look.text,
                                               centred ? TA_TOP : TA_BASELINE, centred ? 0 : at.textBaseline);
        if (centred) CentreInBand(number, size, size);
        Paint(out, number, look.textColour, look.opacity);
    }
    return out;
}

HICON MakeIcon(const BatteryInfo& b, int size, bool lightTaskbar, IconStyle style) {
    const std::vector<uint32_t> pixels = RenderIcon(b, size, lightTaskbar, style);
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

HBITMAP MakeHoverPreview(const BatteryInfo& b, int maxWidth, int maxHeight, bool lightTaskbar) {
    const int w = maxWidth, h = std::min(maxHeight, maxWidth * 56 / 100);
    if (w < 16 || h < 16) return nullptr;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    const ButtonAppearance look = Describe(b, lightTaskbar);
    const uint32_t back = lightTaskbar ? 0xF3F3F3 : 0x2C2C2C;
    const uint32_t fore = Mix(look.glyphColour, back, look.opacity);
    const uint32_t tone = Mix(look.textColour, back, look.opacity);
    const bool known = b.percent >= 0;

    RECT all = {0, 0, w, h};
    HBRUSH backBrush = CreateSolidBrush(ToColorRef(back));
    FillRect(dc, &all, backBrush);
    DeleteObject(backBrush);
    SetBkMode(dc, TRANSPARENT);

    // Top row: the device picture beside the reading, centred as a pair.
    const wchar_t* pictureFace = PictureFace(dc);
    const std::wstring picture(1, pictureFace ? look.glyph : KindLetter(b.kind));
    const std::wstring number = known ? (b.approximate ? L"~" : L"") + look.text + L"%" : look.text;
    HFONT pictureFont = UiFont(pictureFace ? pictureFace : L"Segoe UI", h * 30 / 100, pictureFace ? FW_NORMAL : FW_SEMIBOLD);
    HFONT numberFont = UiFont(L"Segoe UI", h * 40 / 100, FW_SEMIBOLD);
    HFONT noteFont = UiFont(L"Segoe UI", std::max(9, h * 13 / 100), FW_NORMAL);
    if (pictureFont && numberFont && noteFont) {
        const int pictureWidth = TextWidth(dc, pictureFont, picture), numberWidth = TextWidth(dc, numberFont, number);
        const int gap = h * 10 / 100, left = (w - (pictureWidth + gap + numberWidth)) / 2;
        const int rowTop = h * 10 / 100, rowBottom = h * 62 / 100;
        DrawIn(dc, pictureFont, picture, {left, rowTop, left + pictureWidth, rowBottom}, fore, DT_LEFT | DT_VCENTER);
        DrawIn(dc, numberFont, number, {left + pictureWidth + gap, rowTop, w, rowBottom}, tone, DT_LEFT | DT_VCENTER);

        // A level bar under it, then one line of detail.
        if (known) {
            const int barLeft = w * 18 / 100, barRight = w * 82 / 100, barTop = h * 66 / 100;
            const int barBottom = barTop + std::max(4, h * 5 / 100);
            Pill(dc, barLeft, barTop, barRight, barBottom, Mix(fore, back, 50));
            const int filled = (barRight - barLeft) * look.level / 100;
            if (filled > 0) Pill(dc, barLeft, barTop, barLeft + std::max(filled, barBottom - barTop), barBottom, tone);
        }
        const std::wstring note = !known ? b.name
                                  : b.stale ? L"Asleep, last reading"
                                  : b.charging ? L"Charging"
                                  : b.approximate ? L"Approximate" : L"";
        DrawIn(dc, noteFont, note, {0, h * 76 / 100, w, h * 96 / 100}, Mix(fore, back, 180), DT_CENTER | DT_VCENTER);
    }
    if (pictureFont) DeleteObject(pictureFont);
    if (numberFont) DeleteObject(numberFont);
    if (noteFont) DeleteObject(noteFont);
    GdiFlush();

    // GDI leaves the alpha byte at zero; the preview is fully opaque.
    auto* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < w * h; ++i) px[i] |= 0xFF000000u;

    SelectObject(dc, oldBitmap);
    DeleteDC(dc);
    return bitmap;
}
