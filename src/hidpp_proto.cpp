#include "hidpp_proto.h"

#include <algorithm>
#include <chrono>

namespace hidpp {

namespace {

struct VoltagePoint { int mv; int percent; };
// Li-ion discharge curve, as used by Solaar.
const VoltagePoint kVoltageTable[] = {
    {4186, 100}, {4067, 90}, {3989, 80}, {3922, 70}, {3859, 60}, {3811, 50}, {3778, 40},
    {3751, 30}, {3717, 20}, {3671, 10}, {3646, 5}, {3579, 2}, {3500, 0},
};
constexpr size_t kVoltageCount = sizeof(kVoltageTable) / sizeof(kVoltageTable[0]);

}  // namespace

int voltageToPercent(int mv) {
    if (mv >= kVoltageTable[0].mv) return 100;
    if (mv <= kVoltageTable[kVoltageCount - 1].mv) return 0;
    for (size_t i = 1; i < kVoltageCount; ++i) {
        const VoltagePoint& hi = kVoltageTable[i - 1];
        const VoltagePoint& lo = kVoltageTable[i];
        if (mv >= lo.mv) {
            int span = hi.mv - lo.mv;
            int num = (mv - lo.mv) * (hi.percent - lo.percent);
            return lo.percent + (num + span / 2) / span;
        }
    }
    return 0;
}

bool parseUnifiedBattery(const std::vector<uint8_t>& p, Battery& out) {
    if (p.size() < 3) return false;
    int soc = p[0];
    int level = p[1];   // bitmask: 1 critical, 2 low, 4 good, 8 full
    int status = p[2];  // 0 discharging, 1 charging, 2 slow charging, 3 charged, 4 error
    if (soc > 100 || status > 4) return false;
    out.charging = status >= 1 && status <= 3;
    out.approximate = false;
    if (soc == 0 && level != 0) {
        // Device does not report a state of charge, only coarse levels.
        out.percent = (level & 8) ? 90 : (level & 4) ? 50 : (level & 2) ? 20 : 5;
        out.approximate = true;
    } else {
        out.percent = soc;
    }
    return true;
}

bool parseBatteryStatus(const std::vector<uint8_t>& p, Battery& out) {
    if (p.size() < 3) return false;
    int level = p[0];
    int status = p[2];  // 0 discharging, 1 recharging, 2 almost full, 3 full, 4 slow recharge
    if (level > 100 || status > 4) return false;
    out.percent = level;
    out.charging = status >= 1;
    out.approximate = false;
    return true;
}

bool parseBatteryVoltage(const std::vector<uint8_t>& p, Battery& out) {
    if (p.size() < 3) return false;
    int mv = (p[0] << 8) | p[1];
    if (mv == 0) return false;
    out.percent = voltageToPercent(mv);
    out.charging = (p[2] & 0x80) != 0;
    out.approximate = false;
    return true;
}

Kind kindFromType(int t) {
    switch (t) {
        case 0: return Kind::Keyboard;
        case 3: case 4: case 5: return Kind::Mouse;  // mouse, trackpad, trackball
        case 8: return Kind::Headset;
        case 11: case 12: case 17: return Kind::Controller;  // joystick, gamepad, programmable
        default: return Kind::Other;
    }
}

uint8_t Session::nextSwId() {
    swId_ = (swId_ >= 0x0F) ? 0x01 : static_cast<uint8_t>(swId_ + 1);
    return swId_;
}

Reply Session::call(uint8_t devIdx, uint8_t featIdx, uint8_t function,
                    const std::vector<uint8_t>& params) {
    Reply r;
    bool useLong = !t_.canShort || params.size() > kShortLen - 4;
    if ((useLong && !t_.canLong) || !t_.write || !t_.read) return r;
    size_t len = useLong ? kLongLen : kShortLen;

    std::vector<uint8_t> req(len, 0);
    uint8_t sw = nextSwId();
    uint8_t funcSw = static_cast<uint8_t>(((function & 0x0F) << 4) | sw);
    req[0] = useLong ? kReportLong : kReportShort;
    req[1] = devIdx;
    req[2] = featIdx;
    req[3] = funcSw;
    for (size_t i = 0; i < params.size() && 4 + i < len; ++i) req[4 + i] = params[i];

    if (!t_.write(req.data(), req.size())) return r;

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs_);
    uint8_t buf[64];
    for (int guard = 0; guard < 64; ++guard) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (left <= 0) break;
        int n = t_.read(buf, sizeof buf, static_cast<int>(left));
        if (n < 0) return r;
        if (n == 0) break;
        if (n < static_cast<int>(kShortLen)) continue;
        if (buf[0] != kReportShort && buf[0] != kReportLong && buf[0] != 0x12) continue;
        bool devMatch = buf[1] == devIdx || (devIdx == kDirectIndex && buf[1] == 0x00);
        if (!devMatch) continue;
        // Error replies: HID++ 2.0 uses 0xFF, HID++ 1.0 uses 0x8F.
        if ((buf[2] == 0xFF || buf[2] == 0x8F) && n >= 6 && buf[3] == featIdx && buf[4] == funcSw) {
            r.error = buf[5];
            return r;
        }
        if (buf[2] == featIdx && buf[3] == funcSw) {
            r.ok = true;
            r.params.assign(buf + 4, buf + n);
            return r;
        }
        // Anything else is a notification or a stale reply: ignore it.
    }
    r.timedOut = true;
    return r;
}

int Session::featureIndex(uint8_t devIdx, uint16_t featureId) {
    Reply r = call(devIdx, 0x00, 0, {static_cast<uint8_t>(featureId >> 8), static_cast<uint8_t>(featureId & 0xFF)});
    if (!r.ok || r.params.empty()) return -1;
    return r.params[0];
}

bool Session::readBattery(uint8_t devIdx, Battery& out) {
    struct Source { uint16_t feature; uint8_t function; bool (*parse)(const std::vector<uint8_t>&, Battery&); };
    static const Source sources[] = {
        {kFeatUnifiedBattery, 1, parseUnifiedBattery},  // function 0 is getCapabilities
        {kFeatBatteryStatus, 0, parseBatteryStatus},
        {kFeatBatteryVoltage, 0, parseBatteryVoltage},
    };
    for (const Source& s : sources) {
        int idx = featureIndex(devIdx, s.feature);
        if (idx < 0) return false;  // offline or not HID++ 2.0: stop early
        if (idx == 0) continue;
        Reply r = call(devIdx, static_cast<uint8_t>(idx), s.function);
        if (r.ok && s.parse(r.params, out)) return true;
    }
    return false;
}

bool Session::readName(uint8_t devIdx, std::string& out) {
    int idx = featureIndex(devIdx, kFeatDeviceName);
    if (idx <= 0) return false;
    Reply c = call(devIdx, static_cast<uint8_t>(idx), 0);
    if (!c.ok || c.params.empty()) return false;
    size_t count = c.params[0];
    std::string name;
    bool done = false;
    while (!done && name.size() < count) {
        Reply r = call(devIdx, static_cast<uint8_t>(idx), 1, {static_cast<uint8_t>(name.size())});
        if (!r.ok || r.params.empty()) break;
        size_t before = name.size();
        for (uint8_t b : r.params) {
            if (b == 0 || name.size() >= count) { done = true; break; }
            name.push_back(static_cast<char>(b));
        }
        if (name.size() == before) break;
    }
    if (name.empty()) return false;
    out = name;
    return true;
}

int Session::readType(uint8_t devIdx) {
    int idx = featureIndex(devIdx, kFeatDeviceName);
    if (idx <= 0) return -1;
    Reply r = call(devIdx, static_cast<uint8_t>(idx), 2);
    if (!r.ok || r.params.empty()) return -1;
    return r.params[0];
}

bool Session::probe(uint8_t devIdx, DeviceInfo& out) {
    if (!readBattery(devIdx, out.battery)) return false;
    readName(devIdx, out.name);
    int type = readType(devIdx);
    out.kind = type >= 0 ? kindFromType(type) : Kind::Other;
    return true;
}

}  // namespace hidpp
