# BatteryBar

A small Windows utility that shows battery percentages for wireless mice, keyboards, headsets and game controllers as buttons on the taskbar, next to the icons of running apps. Each device gets its own button. In the default style it is an outline picture of the device (mouse, keyboard, headset or controller) above the percentage, with no background, drawn in white on a dark taskbar and near-black on a light one. Colour is kept for the two states that matter: the number turns red at 20 or below and green while the device is charging. A device that is asleep is faded. The pictures come from the icon font that ships with Windows (Segoe Fluent Icons on Windows 11, Segoe MDL2 Assets on Windows 10). Hovering over a button shows a preview with the reading in large type, a level bar and the state (charging, asleep or approximate), whichever icon style is in use. The title above it gives the same detail in words, for example "87% (charging) - MX Master 3S".

It is a single static executable with no installer, frameworks or third-party libraries.

## Download

**[Download BatteryBar.exe](https://github.com/schemafault/batterybar/releases/latest/download/BatteryBar.exe)** (latest release, 64-bit Windows 10 or 11)

Save it anywhere and run it; there is nothing to install. Earlier versions are on the [releases page](https://github.com/schemafault/batterybar/releases).

### If Windows blocks it

The file is not code-signed, so Windows treats a downloaded copy as unknown. What to do depends on which message appears:

- **The browser will not keep the download.** In Edge, open the downloads list, choose "..." next to the file, then "Keep", then "Keep anyway".
- **A blue "Windows protected your PC" box.** Choose "More info", then "Run anyway".
- **No "Run anyway" button.** Right-click BatteryBar.exe, choose Properties, tick "Unblock" at the bottom of the General tab and choose OK. The same thing from PowerShell:

      Unblock-File .\BatteryBar.exe

- **"Smart App Control blocked this app".** Smart App Control refuses every unsigned program and has no per-app exception, so unblocking does not help. BatteryBar will only run with Smart App Control turned off (Windows Security, App & browser control, Smart App Control settings), which affects all apps, not just this one.

### Checking the download

Before unblocking a file, you can confirm it is the one this repository built. Each release from v1.0.1 onwards lists the SHA-256 of the exe in its notes; compare it with:

    Get-FileHash .\BatteryBar.exe

Those releases also carry a build provenance record signed through Sigstore, which shows that the exe was produced by this repository's GitHub Actions workflow and not uploaded by hand. With the [GitHub CLI](https://cli.github.com) installed:

    gh attestation verify .\BatteryBar.exe --repo schemafault/batterybar

## Sources

Devices are read from three places. Where the same device appears in more than one, the first in this list wins.

1. Logitech HID++ 2.0. Talks directly to Logitech devices on a Logi Bolt or Unifying receiver, over Bluetooth LE, or by USB cable. It tries the unified battery, battery status and battery voltage features in turn. This is the most accurate source for MX Master and MX Keys. Logi Options+ usually does not get in the way, but if a device does not show up, try closing it. A device that stops answering (asleep or switched off) keeps its last reading for 30 minutes, marked as such on hover, and is then removed until it wakes.
2. Bluetooth battery property. Any paired Bluetooth device for which Windows exposes a battery level. Headsets only appear if they report their battery to Windows, which many do not.
3. XInput. Wireless Xbox-style controllers. XInput only reports empty, low, medium or full, so the value is shown as an approximation (5, 30, 65 or 100 percent, with a "~" on hover). If a Bluetooth controller is already listed from source 2, XInput results are left out.

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

Run BatteryBar.exe. A button for each device appears on the taskbar. The devices are polled every 60 seconds and again shortly after a device is connected or removed.

Click any button for a menu with:

- the list of devices and their levels
- Refresh now
- Icon style, which switches every button between three designs: the picture above the number (the default), the picture inside a ring that fills with the battery level, or the number inside that ring. Each shows two of picture, number and level, because all three do not fit legibly in a taskbar icon. The choice is kept under HKCU\Software\BatteryBar
- Start with Windows, which adds or removes a value under HKCU\Software\Microsoft\Windows\CurrentVersion\Run for the current user
- Exit

If no device is found, a single placeholder button stays so that the menu is still reachable. "Close window" on a button's right-click menu exits the app. Only one instance runs at a time.

The buttons are minimised windows that never open, so they also appear in Alt+Tab.

## Releases

Every push is built on GitHub Actions and the executable is attached to the run as the BatteryBar artefact. Pushing a tag that starts with "v" (for example v1.0.0) also publishes the executable as a GitHub release, with its SHA-256 in the notes and a signed build provenance record.

## Limits

- The HID++ code has not been checked against every Logitech model. Devices that answer only HID++ 1.0 are not supported.
- Voltage-based percentages (older devices) are estimates from a typical Li-ion curve.
- Battery levels reported by Windows for Bluetooth devices have no charging state, so "charging" is never shown for them.

## Licence

MIT. See LICENSE.
