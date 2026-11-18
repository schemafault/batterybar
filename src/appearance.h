// How a device's taskbar button looks: the picture, the number, their colours
// and where they sit. Pure functions with no Windows types, so every rule here
// can be tested. icon.cpp only has to draw what this describes.
#pragma once

#include "device.h"

#include <cstdint>
#include <string>

struct ButtonAppearance {
    std::wstring label;    // hover/title text, e.g. "MX Master 3S: 87% (charging)"
    std::wstring text;     // what is printed on the icon: "87", or "?" when unknown
    wchar_t glyph;         // the device picture: a code point in Segoe Fluent Icons / Segoe MDL2 Assets
    uint32_t textColour;   // 0xRRGGBB
    uint32_t glyphColour;  // 0xRRGGBB
    uint8_t opacity;       // 255, or less for a device that is asleep
};

// The icon has no background, so its colours depend on the taskbar behind it.
// Colour is kept for the two states that matter: red when low, green when charging.
ButtonAppearance Describe(const BatteryInfo& b, bool lightTaskbar = false);

// Where the picture and the number go in an icon of the given size, in pixels.
// The picture is centred in rows [0, glyphBottom); the number sits on textBaseline.
struct IconLayout {
    int glyphEm;       // font size of the picture
    int glyphBottom;   // first row below the picture's band
    int textEm;        // font size of the number: the same for every reading
    int textBaseline;
};
IconLayout LayoutFor(int size);

wchar_t KindLetter(DeviceKind k);  // one-letter tag used in the menu
