#include "appearance.h"

#include <algorithm>

namespace {

struct Palette {
    uint32_t fore, low, charging;
};
constexpr Palette kOnDark = {0xFFFFFF, 0xFF6B6B, 0x6CCB5F};
constexpr Palette kOnLight = {0x1A1A1A, 0xC42B1C, 0x0F7B0F};

constexpr int kLowPercent = 20;
constexpr uint8_t kAsleepOpacity = 115;  // about 45%

std::wstring Label(const BatteryInfo& b) {
    if (b.percent < 0) return b.name;
    std::wstring t = b.name + L": " + (b.approximate ? L"~" : L"") + std::to_wstring(b.percent) + L"%";
    if (b.charging) t += L" (charging)";
    if (b.stale) t += L" (asleep, last reading)";
    return t;
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
    a.textColour = TextColour(b, p);
    a.glyphColour = p.fore;
    a.opacity = b.stale ? kAsleepOpacity : 255;
    return a;
}

IconLayout LayoutFor(int size) { return {size * 56 / 100, size * 58 / 100, size * 46 / 100, size * 95 / 100}; }

wchar_t KindLetter(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L'M';
        case DeviceKind::Keyboard: return L'K';
        case DeviceKind::Headset: return L'H';
        case DeviceKind::Controller: return L'C';
        default: return L'?';
    }
}
