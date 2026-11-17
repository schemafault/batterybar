// Common types shared by the battery providers and the tray UI.
#pragma once

#include <string>
#include <vector>

enum class DeviceKind { Mouse, Keyboard, Headset, Controller, Other };

struct BatteryInfo {
    std::wstring name;
    DeviceKind kind = DeviceKind::Other;
    int percent = -1;          // 0..100, -1 if unknown
    bool charging = false;
    bool approximate = false;  // coarse value, shown with "~"
    bool stale = false;        // device is silent; this is its last reading
    std::wstring id;           // stable per device, used to track tray icons
};

// Each provider is a blocking function that returns the devices it can see.
// They must be called off the UI thread.
std::vector<BatteryInfo> QueryHidpp();      // hidpp.cpp
std::vector<BatteryInfo> QueryBluetooth();  // btbattery.cpp
std::vector<BatteryInfo> QueryXInput();     // xinput_battery.cpp
