#include "appearance.h"

#include <algorithm>
#include <cmath>

namespace {

struct Palette {
    uint32_t fore, low, charging;
};
constexpr Palette kOnDark = {0xFFFFFF, 0xFF6B6B, 0x6CCB5F};
constexpr Palette kOnLight = {0x1A1A1A, 0xC42B1C, 0x0F7B0F};

constexpr int kLowPercent = 20;
constexpr uint8_t kAsleepOpacity = 115;  // about 45%

// The number comes first: the taskbar's hover preview cuts a long title short,
// and it must not be the reading that gets cut.
std::wstring Label(const BatteryInfo& b) {
    if (b.percent < 0) return b.name;
    std::wstring t = (b.approximate ? L"~" : L"") + std::to_wstring(b.percent) + L"%";
    if (b.charging) t += L" (charging)";
    if (b.stale) t += L" (asleep, last reading)";
    return t + L" - " + b.name;
}

// An asleep device is only faded: its last reading is not a call for attention.
uint32_t TextColour(const BatteryInfo& b, const Palette& p) {
    if (b.percent < 0 || b.stale) return p.fore;
    if (b.charging) return p.charging;
    return b.percent <= kLowPercent ? p.low : p.fore;
}

// Code points shared by Segoe Fluent Icons (Windows 11) and Segoe MDL2 Assets (Windows 10).
wchar_t Glyph(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L'\xE962';       // Mouse
        case DeviceKind::Keyboard: return L'\xE765';    // KeyboardClassic
        case DeviceKind::Headset: return L'\xE7F6';     // Headphone
        case DeviceKind::Controller: return L'\xE7FC';  // Game
        default: return L'\xE83F';                      // Battery10
    }
}

}  // namespace

ButtonAppearance Describe(const BatteryInfo& b, bool lightTaskbar) {
    const Palette& p = lightTaskbar ? kOnLight : kOnDark;
    ButtonAppearance a;
    a.label = Label(b);
    a.text = b.percent < 0 ? L"?" : std::to_wstring(std::min(b.percent, 100));
    a.glyph = Glyph(b.kind);
    a.level = std::max(0, std::min(b.percent, 100));
    a.textColour = TextColour(b, p);
    a.glyphColour = p.fore;
    a.opacity = b.stale ? kAsleepOpacity : 255;
    return a;
}

const wchar_t* StyleName(IconStyle s) {
    switch (s) {
        case IconStyle::PictureInRing: return L"Picture in a ring";
        case IconStyle::NumberInRing: return L"Number in a ring";
        default: return L"Picture and number";
    }
}

IconLayout LayoutFor(int size, IconStyle style, int digits) {
    switch (style) {
        case IconStyle::PictureInRing: return {size * 50 / 100, size, 0, -1, true};
        case IconStyle::NumberInRing: return {0, 0, size * (digits >= 3 ? 36 : 46) / 100, -1, true};
        default: return {size * 56 / 100, size * 58 / 100, size * 46 / 100, size * 95 / 100, false};
    }
}

RingMask RenderRing(int size, int percent) {
    RingMask m;
    if (size <= 0) return m;
    const size_t count = static_cast<size_t>(size) * static_cast<size_t>(size);
    m.track.assign(count, 0);
    m.level.assign(count, 0);

    // Unit coordinates with the centre at 0: the band is 1/12 of the icon wide.
    const float outer = 0.479f, inner = 0.396f, mid = (outer + inner) / 2, half = (outer - inner) / 2;
    const float twoPi = 6.2831853f;
    const float sweep = twoPi * static_cast<float>(std::max(0, std::min(percent, 100))) / 100.0f;
    const bool partial = percent > 0 && percent < 100;
    const float endX = mid * std::sin(sweep), endY = -mid * std::cos(sweep);  // where the filled part stops

    const int sub = 4;  // sub x sub samples per pixel
    const int n = sub * sub;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            int inTrack = 0, inLevel = 0;
            for (int j = 0; j < sub; ++j)
                for (int i = 0; i < sub; ++i) {
                    const float u = (static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / sub) / static_cast<float>(size) - 0.5f;
                    const float v = (static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / sub) / static_cast<float>(size) - 0.5f;
                    const float d = std::sqrt(u * u + v * v);
                    if (d < inner || d > outer) continue;
                    ++inTrack;
                    float angle = std::atan2(u, -v);  // clockwise from the top
                    if (angle < 0) angle += twoPi;
                    bool filled = percent >= 100 || (percent > 0 && angle <= sweep);
                    if (!filled && partial) {  // round both ends of the filled part
                        const float sx = u, sy = v + mid, ex = u - endX, ey = v - endY;
                        filled = sx * sx + sy * sy <= half * half || ex * ex + ey * ey <= half * half;
                    }
                    if (filled) ++inLevel;
                }
            const size_t at = static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x);
            m.track[at] = static_cast<uint8_t>(255 * inTrack / n);
            m.level[at] = static_cast<uint8_t>(255 * inLevel / n);
        }
    return m;
}

wchar_t KindLetter(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L'M';
        case DeviceKind::Keyboard: return L'K';
        case DeviceKind::Headset: return L'H';
        case DeviceKind::Controller: return L'C';
        default: return L'?';
    }
}
