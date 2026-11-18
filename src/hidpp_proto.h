// Platform-independent HID++ 2.0 packet building and parsing.
// No Windows headers here: all I/O goes through the Transport callbacks so the
// logic can be unit-tested with a fake device.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "device.h"

namespace hidpp {

constexpr uint8_t kReportShort = 0x10;  // 7 bytes
constexpr uint8_t kReportLong = 0x11;   // 20 bytes
constexpr size_t kShortLen = 7;
constexpr size_t kLongLen = 20;
constexpr uint8_t kDirectIndex = 0xFF;  // direct Bluetooth / USB devices

// read() returns the number of bytes read, 0 on timeout, negative on error.
struct Transport {
    std::function<bool(const uint8_t*, size_t)> write;
    std::function<int(uint8_t*, size_t, int timeoutMs)> read;
    bool canShort = false;
    bool canLong = true;
};

struct Battery {
    int percent = -1;
    bool charging = false;
    bool approximate = false;
};

struct FoundDevice {
    uint8_t index = 0;  // kDirectIndex, or the receiver slot 1..6
    std::string name;  // empty if the device did not report one
    DeviceKind kind = DeviceKind::Other;
    Battery battery;
};

// Pure parsers. Each returns false if the payload is too short or invalid.
bool parseUnifiedBattery(const std::vector<uint8_t>& p, Battery& out);
bool parseBatteryStatus(const std::vector<uint8_t>& p, Battery& out);
bool parseBatteryVoltage(const std::vector<uint8_t>& p, Battery& out);
int voltageToPercent(int millivolts);
DeviceKind kindFromType(int deviceType);

// Finds the devices reachable through one transport: a direct device at
// kDirectIndex if there is one, otherwise whatever sits in receiver slots 1..6.
// timeoutMs is the wait for each reply.
std::vector<FoundDevice> discover(Transport t, int timeoutMs);

}  // namespace hidpp
