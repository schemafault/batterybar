# BatteryBar

A small Windows utility that shows battery percentages for wireless mice, keyboards, headsets and game controllers as icons in the notification area (system tray). Each device gets its own icon showing the percentage in a colour by level (green above 50, amber 21 to 50, red 20 or below). A coloured band along the bottom edge shows the kind of device: blue for mouse, violet for keyboard, pink for headset, white for controller. A white band along the top means the device is charging. The tooltip gives the full detail, for example "MX Master 3S: 87% (charging)".

It is a single static executable with no installer, frameworks or third-party libraries.

## Sources

Devices are read from three places. Where the same device appears in more than one, the first in this list wins.

1. Logitech HID++ 2.0. Talks directly to Logitech devices on a Logi Bolt or Unifying receiver, over Bluetooth LE, or by USB cable. It tries the unified battery, battery status and battery voltage features in turn. This is the most accurate source for MX Master and MX Keys. Logi Options+ usually does not get in the way, but if a device does not show up, try closing it. A device that stops answering (asleep or switched off) keeps its last reading for 30 minutes, marked as such in the tooltip, and is then removed until it wakes.
2. Bluetooth battery property. Any paired Bluetooth device for which Windows exposes a battery level. Headsets only appear if they report their battery to Windows, which many do not.
3. XInput. Wireless Xbox-style controllers. XInput only reports empty, low, medium or full, so the value is shown as an approximation (5, 30, 65 or 100 percent, with a "~" in the tooltip). If a Bluetooth controller is already listed from source 2, XInput results are left out.

## Build

Requires CMake 3.16 or later and a C++17 compiler.

With Visual Studio (static runtime, x64):

    cmake -S . -B build -A x64
    cmake --build build --config Release
    ctest --test-dir build -C Release --output-on-failure

The executable is build/Release/BatteryBar.exe.

With MinGW-w64 on Windows (MSYS2):

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build

Cross-compiling from Linux with MinGW-w64:

    cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
    cmake --build build-win

On a non-Windows host without a cross toolchain, only the protocol tests are configured:

    cmake -S . -B build && cmake --build build && ctest --test-dir build

## Usage

Run BatteryBar.exe. Icons appear in the notification area; Windows may place them in the overflow menu at first, so drag them out to keep them visible. The devices are polled every 60 seconds and again shortly after a device is connected or removed.

Right-click any icon for a menu with:

- the list of devices and their levels
- Refresh now
- Start with Windows, which adds or removes a value under HKCU\Software\Microsoft\Windows\CurrentVersion\Run for the current user
- Exit

If no device is found, a single placeholder icon stays so that the menu is still reachable. Only one instance runs at a time.

## Limits

- The HID++ code has not been checked against every Logitech model. Devices that answer only HID++ 1.0 are not supported.
- Voltage-based percentages (older devices) are estimates from a typical Li-ion curve.
- Battery levels reported by Windows for Bluetooth devices have no charging state, so "charging" is never shown for them.

## Licence

MIT. See LICENSE.
