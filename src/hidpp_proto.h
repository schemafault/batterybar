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

namespace hidpp {

constexpr uint8_t kReportShort = 0x10;  // 7 bytes
constexpr uint8_t kReportLong = 0x11;   // 20 bytes
constexpr size_t kShortLen = 7;
constexpr size_t kLongLen = 20;
constexpr uint8_t kDirectIndex = 0xFF;  // direct Bluetooth / USB devices

constexpr uint16_t kFeatDeviceName = 0x0005;
constexpr uint16_t kFeatBatteryStatus = 0x1000;
constexpr uint16_t kFeatBatteryVoltage = 0x1001;
constexpr uint16_t kFeatUnifiedBattery = 0x1004;

// read() returns the number of bytes read, 0 on timeout, negative on error.
struct Transport {
    std::function<bool(const uint8_t*, size_t)> write;
    std::function<int(uint8_t*, size_t, int timeoutMs)> read;
    bool canShort = false;
    bool canLong = true;
};

struct Reply {
    bool ok = false;
    bool timedOut = false;
    uint8_t error = 0;              // error code when an error reply arrived
    std::vector<uint8_t> params;    // bytes after the 4-byte header
};

struct Battery {
    int percent = -1;
    bool charging = false;
    bool approximate = false;
};

enum class Kind { Keyboard, Mouse, Headset, Controller, Other };

struct DeviceInfo {
    std::string name;
    Kind kind = Kind::Other;
    Battery battery;
};

// Pure parsers. Each returns false if the payload is too short or invalid.
bool parseUnifiedBattery(const std::vector<uint8_t>& p, Battery& out);
bool parseBatteryStatus(const std::vector<uint8_t>& p, Battery& out);
bool parseBatteryVoltage(const std::vector<uint8_t>& p, Battery& out);
int voltageToPercent(int millivolts);
Kind kindFromType(int deviceType);

class Session {
public:
    explicit Session(Transport t, int timeoutMs = 500) : t_(std::move(t)), timeoutMs_(timeoutMs) {}

    // Sends one request and waits for the matching reply, skipping unrelated
    // notifications. params longer than 3 bytes force a long report.
    Reply call(uint8_t devIdx, uint8_t featIdx, uint8_t function,
               const std::vector<uint8_t>& params = {});

    // Returns the feature index, 0 if unsupported, -1 on timeout or error.
    int featureIndex(uint8_t devIdx, uint16_t featureId);
    bool readBattery(uint8_t devIdx, Battery& out);
    bool readName(uint8_t devIdx, std::string& out);
    int readType(uint8_t devIdx);  // -1 if unknown
    // Battery is mandatory; name and type are best effort.
    bool probe(uint8_t devIdx, DeviceInfo& out);

private:
    uint8_t nextSwId();
    Transport t_;
    int timeoutMs_;
    uint8_t swId_ = 0x09;
};

}  // namespace hidpp
