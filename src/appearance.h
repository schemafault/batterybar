// How a device's taskbar button looks: colours, text and the icon picture.
// Pure functions with no Windows types, so every rule here can be tested.
// main.cpp only has to draw the number and turn the pixels into an HICON.
#pragma once

#include "device.h"

#include <cstdint>
#include <string>
#include <vector>

struct ButtonAppearance {
    std::wstring label;   // hover/title text, e.g. "MX Master 3S: 87% (charging)"
    std::wstring text;    // what is printed on the icon: "87", or "?" when unknown
    uint32_t textColour;  // 0xRRGGBB
    uint32_t glyphColour; // 0xRRGGBB
    uint32_t backColour;  // 0xRRGGBB
};

ButtonAppearance Describe(const BatteryInfo& b);

// The icon without its number: a rounded dark square with the device picture
// in the top half. size*size pixels, row-major from the top, 0xAARRGGBB with
// straight (non-premultiplied) alpha. Alpha is the square's edge coverage.
std::vector<uint32_t> RenderBackdrop(const BatteryInfo& b, int size);

// Rows [top, bottom) of an icon of the given size reserved for the number.
struct TextBand {
    int top, bottom;
};
TextBand TextBandFor(int size);

wchar_t KindLetter(DeviceKind k);  // one-letter tag used in the menu
