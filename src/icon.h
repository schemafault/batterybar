// Draws a device's taskbar icon. What it shows is decided in appearance.h;
// this is the part that needs Windows fonts.
#pragma once

#include "device.h"

#include <windows.h>

#include <cstdint>
#include <vector>

// size*size pixels, row-major from the top, 0xAARRGGBB with straight
// (non-premultiplied) alpha. Empty if the icon could not be drawn.
std::vector<uint32_t> RenderIcon(const BatteryInfo& b, int size, bool lightTaskbar);

HICON MakeIcon(const BatteryInfo& b, int size, bool lightTaskbar);
