// Draws a device's taskbar icon. What it shows is decided in appearance.h;
// this is the part that needs Windows fonts.
#pragma once

#include "appearance.h"
#include "device.h"

#include <windows.h>

#include <cstdint>
#include <vector>

// size*size pixels, row-major from the top, 0xAARRGGBB with straight
// (non-premultiplied) alpha. Empty if the icon could not be drawn.
std::vector<uint32_t> RenderIcon(const BatteryInfo& b, int size, bool lightTaskbar, IconStyle style);

HICON MakeIcon(const BatteryInfo& b, int size, bool lightTaskbar, IconStyle style);

// The picture Windows shows when the pointer rests on the taskbar button: the
// reading in large type, whatever the icon style. No larger than maxWidth by
// maxHeight. The caller owns the bitmap.
HBITMAP MakeHoverPreview(const BatteryInfo& b, int maxWidth, int maxHeight, bool lightTaskbar);
