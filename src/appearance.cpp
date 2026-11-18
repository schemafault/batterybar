#include "appearance.h"

#include <algorithm>

namespace {

constexpr uint32_t Rgb(uint32_t r, uint32_t g, uint32_t b) { return (r << 16) | (g << 8) | b; }

uint32_t LevelColour(const BatteryInfo& b) {
    if (b.percent < 0 || b.stale) return Rgb(170, 170, 170);
    if (b.percent > 50) return Rgb(80, 220, 100);
    if (b.percent > 20) return Rgb(255, 193, 7);
    return Rgb(255, 82, 82);
}

std::wstring Label(const BatteryInfo& b) {
    if (b.percent < 0) return b.name;
    std::wstring t = b.name + L": " + (b.approximate ? L"~" : L"") + std::to_wstring(b.percent) + L"%";
    if (b.charging) t += L" (charging)";
    if (b.stale) t += L" (asleep, last reading)";
    return t;
}

// Shapes are described in unit coordinates (0..1 across the icon) and
// rasterised by supersampling, so they stay smooth at any icon size.
bool InRoundRect(float u, float v, float x0, float y0, float x1, float y1, float r) {
    if (u < x0 || u > x1 || v < y0 || v > y1) return false;
    float cx = u < x0 + r ? x0 + r : (u > x1 - r ? x1 - r : u);
    float cy = v < y0 + r ? y0 + r : (v > y1 - r ? y1 - r : v);
    float dx = u - cx, dy = v - cy;
    return dx * dx + dy * dy <= r * r;
}

bool InCircle(float u, float v, float cx, float cy, float r) {
    float dx = u - cx, dy = v - cy;
    return dx * dx + dy * dy <= r * r;
}

// The device picture, in the top half of the icon.
bool InGlyph(DeviceKind k, float u, float v) {
    switch (k) {
        case DeviceKind::Mouse:  // body with a scroll wheel cut out
            return InRoundRect(u, v, 0.35f, 0.07f, 0.65f, 0.47f, 0.15f) &&
                   !InRoundRect(u, v, 0.465f, 0.13f, 0.535f, 0.25f, 0.035f);
        case DeviceKind::Keyboard: {  // board with a row of keys and a space bar cut out
            if (!InRoundRect(u, v, 0.09f, 0.11f, 0.91f, 0.45f, 0.06f)) return false;
            if (v >= 0.18f && v <= 0.255f)
                for (int i = 0; i < 5; ++i) {
                    float x = 0.17f + 0.14f * static_cast<float>(i);
                    if (u >= x && u <= x + 0.095f) return false;
                }
            if (v >= 0.31f && v <= 0.385f) {
                if (u >= 0.17f && u <= 0.265f) return false;
                if (u >= 0.31f && u <= 0.69f) return false;
                if (u >= 0.735f && u <= 0.83f) return false;
            }
            return true;
        }
        case DeviceKind::Headset:  // band and two ear cups
            return (v <= 0.30f && InCircle(u, v, 0.5f, 0.30f, 0.22f) && !InCircle(u, v, 0.5f, 0.30f, 0.16f)) ||
                   InRoundRect(u, v, 0.25f, 0.27f, 0.37f, 0.46f, 0.04f) ||
                   InRoundRect(u, v, 0.63f, 0.27f, 0.75f, 0.46f, 0.04f);
        case DeviceKind::Controller:  // pad with two sticks cut out
            return InRoundRect(u, v, 0.13f, 0.13f, 0.87f, 0.45f, 0.14f) && !InCircle(u, v, 0.32f, 0.28f, 0.055f) &&
                   !InCircle(u, v, 0.68f, 0.28f, 0.055f);
        default:  // a battery
            return InRoundRect(u, v, 0.20f, 0.16f, 0.76f, 0.42f, 0.04f) || InRoundRect(u, v, 0.76f, 0.23f, 0.82f, 0.35f, 0.01f);
    }
}

const uint32_t kBack = 0x202020;

uint32_t GlyphColour(const BatteryInfo& b) { return b.stale ? 0x8A8A8A : (b.charging ? 0xFFD54F : 0xF0F0F0); }

}  // namespace

ButtonAppearance Describe(const BatteryInfo& b) {
    ButtonAppearance a;
    a.label = Label(b);
    a.text = b.percent < 0 ? L"?" : std::to_wstring(std::min(b.percent, 100));
    a.textColour = LevelColour(b);
    a.glyphColour = GlyphColour(b);
    a.backColour = kBack;
    return a;
}

TextBand TextBandFor(int size) { return {size * 46 / 100, size * 98 / 100}; }

std::vector<uint32_t> RenderBackdrop(const BatteryInfo& b, int size) {
    std::vector<uint32_t> out;
    if (size <= 0) return out;
    out.assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0u);

    const uint32_t back = kBack;
    const uint32_t fore = GlyphColour(b);
    const int bandTop = TextBandFor(size).top;
    const int sub = 4;  // sub x sub samples per pixel
    const int n = sub * sub;
    const float fs = static_cast<float>(size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inBack = 0, inFore = 0;
            for (int j = 0; j < sub; ++j)
                for (int i = 0; i < sub; ++i) {
                    float u = (static_cast<float>(x) + (static_cast<float>(i) + 0.5f) / sub) / fs;
                    float v = (static_cast<float>(y) + (static_cast<float>(j) + 0.5f) / sub) / fs;
                    if (!InRoundRect(u, v, 0.0f, 0.0f, 1.0f, 1.0f, 0.18f)) continue;
                    ++inBack;
                    // The picture must never reach the rows the number is drawn in.
                    if (y < bandTop && InGlyph(b.kind, u, v)) ++inFore;
                }
            if (inBack == 0) continue;  // outside the square: stays 0
            uint32_t c = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                uint32_t lo = (back >> shift) & 0xFF, hi = (fore >> shift) & 0xFF;
                c |= ((lo * static_cast<uint32_t>(n - inFore) + hi * static_cast<uint32_t>(inFore)) / n) << shift;
            }
            uint32_t a = static_cast<uint32_t>(255 * inBack / n);
            out[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] = c | (a << 24);
        }
    }
    return out;
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
