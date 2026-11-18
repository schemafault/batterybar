// Decides which devices to show from one poll's readings. No platform calls,
// so the merge rules can be tested on their own.
#pragma once

#include "device.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// What each provider returned in one poll.
struct ProviderReadings {
    std::vector<BatteryInfo> hidpp, bluetooth, xinput;
};

class Roster {
public:
    // Merges one poll's readings into the list of devices to show, preferring
    // HID++ over Bluetooth over XInput. nowMs is a monotonic clock.
    std::vector<BatteryInfo> update(const ProviderReadings& readings, uint64_t nowMs);

private:
    // A sleeping Logitech device stops answering until it is next used, so
    // its last reading is kept for a while. Maps id to (reading, last seen).
    std::map<std::wstring, std::pair<BatteryInfo, uint64_t>> seen_;
};
