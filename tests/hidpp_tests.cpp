// Plain assert-based tests for the HID++ protocol layer, using a fake device.
// Everything goes through discover(), the module's only entry point.
#include "../src/hidpp_proto.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>

using namespace hidpp;
using Packet = std::vector<uint8_t>;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

// A scripted device: the handler sees each request and may queue replies.
struct Fake {
    std::function<void(const Packet& req, std::deque<Packet>& out)> handler;
    std::deque<Packet> queue;
    std::vector<Packet> sent;
    bool canShort = true, canLong = true;

    Transport transport() {
        Transport t;
        t.canShort = canShort;
        t.canLong = canLong;
        t.write = [this](const uint8_t* d, size_t n) {
            sent.emplace_back(d, d + n);
            if (handler) handler(sent.back(), queue);
            return true;
        };
        t.read = [this](uint8_t* buf, size_t cap, int) {
            if (queue.empty()) return 0;
            Packet p = queue.front();
            queue.pop_front();
            size_t n = p.size() < cap ? p.size() : cap;
            for (size_t i = 0; i < n; ++i) buf[i] = p[i];
            return static_cast<int>(n);
        };
        return t;
    }

    // Models devices by index. Each has unified battery at feature 4 and
    // device name at feature 3.
    struct Dev {
        std::string name = "Test Device";
        int type = 3;                            // mouse
        std::vector<uint8_t> battery{64, 4, 0};  // soc, level, status
        int ignorePings = 0;                     // slow wake-up: swallow this many pings
    };
    std::map<uint8_t, Dev> devs;
    bool errorForMissing = false;  // empty receiver slots answer with a HID++ 1.0 error

    void useDevices() {
        handler = [this](const Packet& r, std::deque<Packet>& q) { respond(r, q); };
    }

private:
    void respond(const Packet& r, std::deque<Packet>& q) {
        auto it = devs.find(r[1]);
        if (it == devs.end()) {
            if (errorForMissing) {  // short report: 10 idx 8F featIdx funcSw 09 00
                Packet p(7, 0);
                p[0] = 0x10; p[1] = r[1]; p[2] = 0x8F; p[3] = r[2]; p[4] = r[3]; p[5] = 0x09;
                q.push_back(p);
            }
            return;
        }
        Dev& d = it->second;
        int fn = r[3] >> 4;
        if (r[2] == 0 && fn == 0) {
            if (r[4] == 0 && r[5] == 1) {  // ping: IRoot.getFeature(0x0001) is how the app pings
                if (d.ignorePings > 0) { --d.ignorePings; return; }
                q.push_back(reply(r, {1}));
                return;
            }
            uint16_t id = static_cast<uint16_t>((r[4] << 8) | r[5]);
            if (id == 0x1004) q.push_back(reply(r, {4}));
            else if (id == 0x0005) q.push_back(reply(r, {3}));
            else q.push_back(reply(r, {0}));  // unsupported
        } else if (r[2] == 4 && fn == 1) {
            Packet p = reply(r, {});
            for (size_t i = 0; i < d.battery.size(); ++i) p[4 + i] = d.battery[i];
            q.push_back(p);
        } else if (r[2] == 3) {
            if (fn == 0) {
                q.push_back(reply(r, {static_cast<uint8_t>(d.name.size())}));
            } else if (fn == 1) {
                Packet p = reply(r, {});
                for (size_t i = 0; i < 16 && r[4] + i < d.name.size(); ++i)
                    p[4 + i] = static_cast<uint8_t>(d.name[r[4] + i]);
                q.push_back(p);
            } else if (fn == 2) {
                q.push_back(reply(r, {static_cast<uint8_t>(d.type)}));
            }
        }
    }

    static Packet reply(const Packet& req, std::initializer_list<uint8_t> params) {
        Packet p(20, 0);
        p[0] = 0x11;
        p[1] = req[1];
        p[2] = req[2];
        p[3] = req[3];
        size_t i = 4;
        for (uint8_t b : params) p[i++] = b;
        return p;
    }
};

static std::vector<uint8_t> indexes(const std::vector<FoundDevice>& v) {
    std::vector<uint8_t> r;
    for (const FoundDevice& d : v) r.push_back(d.index);
    return r;
}

static void test_parsers() {
    Battery b;
    CHECK(parseUnifiedBattery({87, 4, 0}, b) && b.percent == 87 && !b.charging && !b.approximate);
    CHECK(parseUnifiedBattery({55, 4, 1}, b) && b.charging);
    CHECK(parseUnifiedBattery({100, 8, 3}, b) && b.charging && b.percent == 100);
    CHECK(parseUnifiedBattery({0, 2, 0}, b) && b.percent == 20 && b.approximate);
    CHECK(!parseUnifiedBattery({101, 4, 0}, b));
    CHECK(!parseUnifiedBattery({50, 4}, b));

    CHECK(parseBatteryStatus({42, 30, 0}, b) && b.percent == 42 && !b.charging);
    CHECK(parseBatteryStatus({80, 90, 1}, b) && b.charging);
    CHECK(parseBatteryStatus({100, 100, 3}, b) && b.charging);
    CHECK(!parseBatteryStatus({150, 0, 0}, b));

    CHECK(parseBatteryVoltage({0x10, 0x7A, 0x00}, b) && b.percent == 100);  // 4218 mV
    CHECK(parseBatteryVoltage({0x0F, 0x1E, 0x00}, b) && b.percent == voltageToPercent(3870) && !b.charging);
}

static void test_voltage() {
    CHECK(voltageToPercent(4186) == 100);
    CHECK(voltageToPercent(5000) == 100);
    CHECK(voltageToPercent(3500) == 0);
    CHECK(voltageToPercent(3000) == 0);
    CHECK(voltageToPercent(4067) == 90);
    CHECK(voltageToPercent(3811) == 50);
    CHECK(voltageToPercent(3646) == 5);
    // Midpoints between table entries.
    CHECK(voltageToPercent((4186 + 4067) / 2) == 95);   // 4126 -> 95 (rounded)
    CHECK(voltageToPercent((3811 + 3778) / 2) == 45);   // 3794
    CHECK(voltageToPercent((3579 + 3500) / 2) == 1);    // 3539
    // Monotonic.
    int prev = 0;
    for (int mv = 3400; mv <= 4300; ++mv) {
        int p = voltageToPercent(mv);
        CHECK(p >= prev && p >= 0 && p <= 100);
        prev = p;
    }
}

static void test_kind_mapping() {
    CHECK(kindFromType(3) == DeviceKind::Mouse && kindFromType(0) == DeviceKind::Keyboard);
    CHECK(kindFromType(8) == DeviceKind::Headset && kindFromType(12) == DeviceKind::Controller);
    CHECK(kindFromType(99) == DeviceKind::Other);
}

static void test_direct_device_skips_slots() {
    Fake f;
    f.devs[kDirectIndex] = {};
    f.devs[2] = {};  // would be found if slots were walked
    f.errorForMissing = true;
    f.useDevices();
    auto found = discover(f.transport(), 100);
    CHECK(found.size() == 1 && found[0].index == kDirectIndex);
    for (const Packet& p : f.sent) CHECK(p[1] == kDirectIndex);
}

static void test_receiver_slots() {
    Fake f;
    f.devs[1].name = "Keyboard One";
    f.devs[1].type = 0;
    f.devs[3].name = "Mouse Three";
    f.errorForMissing = true;
    f.useDevices();
    auto found = discover(f.transport(), 100);
    CHECK((indexes(found) == std::vector<uint8_t>{1, 3}));
    CHECK(found[0].name == "Keyboard One" && found[0].kind == DeviceKind::Keyboard);
    CHECK(found[1].name == "Mouse Three" && found[1].kind == DeviceKind::Mouse);
}

static void test_error_reply_gives_up_at_once() {
    // An error must not be retried: one ping for 0xFF and one per empty slot.
    Fake f;
    f.errorForMissing = true;
    f.useDevices();
    CHECK(discover(f.transport(), 100).empty());
    CHECK(f.sent.size() == 7);
}

static void test_slow_wake_device_found() {
    Fake f;
    f.devs[kDirectIndex].ignorePings = 1;  // first ping goes unanswered
    f.useDevices();
    auto found = discover(f.transport(), 100);
    CHECK(found.size() == 1 && found[0].index == kDirectIndex && found[0].battery.percent == 64);

    Fake g;  // a receiver slot can wake slowly too
    g.devs[2].ignorePings = 1;
    g.errorForMissing = true;
    g.useDevices();
    CHECK((indexes(discover(g.transport(), 100)) == std::vector<uint8_t>{2}));
}

static void test_silent_device_not_found() {
    Fake f;
    f.devs[kDirectIndex].ignorePings = 100;  // never answers
    f.useDevices();
    CHECK(discover(f.transport(), 100).empty());
    Fake g;  // nothing at all, no errors either
    g.useDevices();
    CHECK(discover(g.transport(), 100).empty());
}

static void test_device_details() {
    const std::string full = "Logitech MX Master 3S Wireless";  // 30 chars: two 16-byte chunks
    Fake f;
    f.devs[kDirectIndex].name = full;
    f.devs[kDirectIndex].type = 8;
    f.devs[kDirectIndex].battery = {55, 4, 1};  // charging
    f.useDevices();
    auto found = discover(f.transport(), 100);
    CHECK(found.size() == 1);
    CHECK(found[0].name == full && found[0].kind == DeviceKind::Headset);
    CHECK(found[0].battery.percent == 55 && found[0].battery.charging && !found[0].battery.approximate);
}

static void test_no_battery_not_found() {
    // Battery is mandatory: a device that answers but reports a bad battery is skipped.
    Fake f;
    f.devs[kDirectIndex].battery = {101, 4, 0};  // invalid payload
    f.useDevices();
    CHECK(discover(f.transport(), 100).empty());
}

static void test_report_selection() {
    auto first = [](bool canShort, bool canLong) {
        Fake f;
        f.canShort = canShort;
        f.canLong = canLong;
        discover(f.transport(), 100);
        return f.sent;
    };
    auto a = first(true, true);  // the ping fits a short report
    CHECK(!a.empty() && a[0].size() == 7 && a[0][0] == 0x10);
    CHECK(a[0][1] == kDirectIndex && a[0][2] == 0 && (a[0][3] >> 4) == 0 && (a[0][3] & 0x0F) != 0);
    auto b = first(false, true);  // long-only device
    CHECK(!b.empty() && b[0].size() == 20 && b[0][0] == 0x11 && b[0][4] == 0 && b[0][5] == 1);
    auto c = first(true, false);
    CHECK(!c.empty() && c[0].size() == 7 && c[0][0] == 0x10);
    auto d = first(false, false);  // cannot send anything
    CHECK(d.empty());
}

static void test_notifications_ignored() {
    Fake f;
    f.devs[kDirectIndex] = {};
    f.useDevices();
    auto inner = f.handler;
    f.handler = [inner](const Packet& r, std::deque<Packet>& q) {
        Packet note(20, 0);  // unrelated notification, swId 0
        note[0] = 0x11; note[1] = r[1]; note[2] = 0x06; note[3] = 0x00;
        q.push_back(note);
        Packet other(20, 0);  // right header but for another device
        other[0] = 0x11; other[1] = static_cast<uint8_t>(r[1] - 1); other[2] = r[2]; other[3] = r[3]; other[4] = 9;
        q.push_back(other);
        Packet wrongSw(20, 0);  // same request, wrong software id
        wrongSw[0] = 0x11; wrongSw[1] = r[1]; wrongSw[2] = r[2];
        wrongSw[3] = static_cast<uint8_t>(r[3] ^ 0x01); wrongSw[4] = 8;
        q.push_back(wrongSw);
        q.push_back(Packet{0x20, 0, 0, 0, 0, 0, 0});  // DJ report: wrong id
        inner(r, q);
    };
    auto found = discover(f.transport(), 100);
    CHECK(found.size() == 1 && found[0].battery.percent == 64);
}

int main() {
    test_parsers();
    test_voltage();
    test_kind_mapping();
    test_direct_device_skips_slots();
    test_receiver_slots();
    test_error_reply_gives_up_at_once();
    test_slow_wake_device_found();
    test_silent_device_not_found();
    test_device_details();
    test_no_battery_not_found();
    test_report_selection();
    test_notifications_ignored();
    std::puts("all hidpp tests passed");
    return 0;
}
