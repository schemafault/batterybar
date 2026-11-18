// How a device's taskbar button looks: the picture, the number, their colours
// and where they sit. Pure functions with no Windows types, so every rule here
// can be tested. icon.cpp only has to draw what this describes.
#pragma once

#include "device.h"

#include <cstdint>
#include <string>
#include <vector>

// The three icon designs the user can choose between in the menu.
enum class IconStyle { PictureAndNumber, PictureInRing, NumberInRing };
constexpr int kIconStyleCount = 3;
const wchar_t* StyleName(IconStyle s);  // as shown in the menu

struct ButtonAppearance {
    std::wstring label;    // hover/title text, e.g. "87% (charging) - MX Master 3S"
    std::wstring text;     // what is printed on the icon: "87", or "?" when unknown
    wchar_t glyph;         // the device picture: a code point in Segoe Fluent Icons / Segoe MDL2 Assets
    int level;             // 0..100 for the ring; 0 when unknown
    uint32_t textColour;   // 0xRRGGBB: the number, and the filled part of the ring
    uint32_t glyphColour;  // 0xRRGGBB
    uint8_t opacity;       // 255, or less for a device that is asleep
};

// The icon has no background, so its colours depend on the taskbar behind it.
// Colour is kept for the two states that matter: red when low, green when charging.
ButtonAppearance Describe(const BatteryInfo& b, bool lightTaskbar = false);

// Where the picture and the number go in an icon of the given size, in pixels.
// Each style shows two of picture, number and ring; the missing one is zero/false.
struct IconLayout {
    int glyphEm;       // font size of the picture; 0 for no picture
    int glyphBottom;   // the picture is centred in rows [0, glyphBottom)
    int textEm;        // font size of the number; 0 for no number
    int textBaseline;  // row the number sits on; -1 to centre it in the icon
    bool ring;
};
// digits is the length of the number: only a number inside a ring shrinks to fit three.
IconLayout LayoutFor(int size, IconStyle style = IconStyle::PictureAndNumber, int digits = 2);

// The ring around the icon, as how much of each pixel it covers (0..255),
// size*size row-major from the top. `track` is the whole circle; `level` is
// the part filled clockwise from the top for the given percentage.
struct RingMask {
    std::vector<uint8_t> track, level;
};
RingMask RenderRing(int size, int percent);

wchar_t KindLetter(DeviceKind k);  // one-letter tag used in the menu
