// Plain assert-based tests for the HID++ protocol layer, using a fake device.
#include "../src/hidpp_proto.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
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
};

static Packet reply(const Packet& req, std::initializer_list<uint8_t> params, uint8_t reportId = 0x11) {
    Packet p(reportId == 0x10 ? 7 : 20, 0);
    p[0] = reportId;
    p[1] = req[1];
    p[2] = req[2];
    p[3] = req[3];
    size_t i = 4;
    for (uint8_t b : params) p[i++] = b;
    return p;
}

static bool isRootGet(const Packet& r, uint16_t id) {
    return r[2] == 0 && (r[3] >> 4) == 0 && r[4] == (id >> 8) && r[5] == (id & 0xFF);
}

static void test_feature_lookup() {
    Fake f;
    f.handler = [](const Packet& r, std::deque<Packet>& q) {
        if (isRootGet(r, 0x1004)) q.push_back(reply(r, {5, 0, 1}));
        else if (isRootGet(r, 0x1000)) q.push_back(reply(r, {0, 0, 0}));  // unsupported
    };
    Session s(f.transport());
    CHECK(s.featureIndex(1, 0x1004) == 5);
    CHECK(s.featureIndex(1, 0x1000) == 0);
    CHECK(s.featureIndex(1, 0x9999) == -1);  // no reply: timeout
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

static void test_battery_via_session() {
    // Device without 0x1004 or 0x1000, with 0x1001 at index 7.
    Fake f;
    f.handler = [](const Packet& r, std::deque<Packet>& q) {
        if (isRootGet(r, 0x1001)) q.push_back(reply(r, {7, 0, 0}));
        else if (isRootGet(r, 0x1004) || isRootGet(r, 0x1000)) q.push_back(reply(r, {0, 0, 0}));
        else if (r[2] == 7 && (r[3] >> 4) == 0) q.push_back(reply(r, {0x0F, 0x1E, 0x80}));  // 3870 mV, charging
    };
    Session s(f.transport());
    Battery b;
    CHECK(s.readBattery(2, b));
    CHECK(b.percent == voltageToPercent(3870) && b.charging);

    // Unified battery uses function 1.
    Fake g;
    g.handler = [](const Packet& r, std::deque<Packet>& q) {
        if (isRootGet(r, 0x1004)) q.push_back(reply(r, {4, 0, 0}));
        else if (r[2] == 4 && (r[3] >> 4) == 1) q.push_back(reply(r, {64, 4, 0}));
    };
    Session s2(g.transport());
    CHECK(s2.readBattery(0xFF, b) && b.percent == 64 && !b.charging);
}

static void test_errors() {
    Fake f;
    f.handler = [](const Packet& r, std::deque<Packet>& q) {
        if (r[1] == 1) {  // HID++ 2.0 error: feature 0xFF, then featIdx, func|sw, code
            Packet p(20, 0);
            p[0] = 0x11; p[1] = r[1]; p[2] = 0xFF; p[3] = r[2]; p[4] = r[3]; p[5] = 0x02;
            q.push_back(p);
        } else if (r[1] == 2) {  // HID++ 1.0 error: sub-id 0x8F
            Packet p(7, 0);
            p[0] = 0x10; p[1] = r[1]; p[2] = 0x8F; p[3] = r[2]; p[4] = r[3]; p[5] = 0x09;
            q.push_back(p);
        }
    };
    Session s(f.transport());
    Reply r1 = s.call(1, 0, 0, {0x10, 0x04});
    CHECK(!r1.ok && !r1.timedOut && r1.error == 0x02);
    Reply r2 = s.call(2, 0, 0, {0x10, 0x04});
    CHECK(!r2.ok && !r2.timedOut && r2.error == 0x09);
    Reply r3 = s.call(3, 0, 0, {0x10, 0x04});
    CHECK(!r3.ok && r3.timedOut);
    Battery b;
    CHECK(!s.readBattery(1, b));
}

static void test_report_selection() {
    auto run = [](bool canShort, bool canLong, size_t nParams) {
        Fake f;
        f.canShort = canShort;
        f.canLong = canLong;
        Session s(f.transport());
        s.call(1, 0, 1, Packet(nParams, 0xAA));
        return f.sent;
    };
    auto a = run(true, true, 2);
    CHECK(a.size() == 1 && a[0].size() == 7 && a[0][0] == 0x10);
    auto b = run(true, true, 4);
    CHECK(b[0].size() == 20 && b[0][0] == 0x11);
    auto c = run(false, true, 2);  // long-only device
    CHECK(c[0].size() == 20 && c[0][0] == 0x11 && c[0][4] == 0xAA);
    auto d = run(true, false, 2);
    CHECK(d[0].size() == 7 && d[0][0] == 0x10);
    auto e = run(true, false, 5);  // cannot send: nothing written
    CHECK(e.empty());
    // Header layout and non-zero software id.
    CHECK(a[0][1] == 1 && a[0][2] == 0 && (a[0][3] >> 4) == 1 && (a[0][3] & 0x0F) != 0);
}

static void test_name_assembly() {
    const std::string full = "Logitech MX Master 3S Wireless";  // 30 chars: two 16-byte chunks
    Fake f;
    f.handler = [&full](const Packet& r, std::deque<Packet>& q) {
        if (isRootGet(r, 0x0005)) { q.push_back(reply(r, {3, 0, 0})); return; }
        if (r[2] != 3) return;
        int fn = r[3] >> 4;
        if (fn == 0) q.push_back(reply(r, {static_cast<uint8_t>(full.size())}));
        else if (fn == 1) {
            Packet p = reply(r, {});
            for (size_t i = 0; i < 16 && r[4] + i < full.size(); ++i) p[4 + i] = static_cast<uint8_t>(full[r[4] + i]);
            q.push_back(p);
        } else if (fn == 2) q.push_back(reply(r, {3}));
    };
    Session s(f.transport());
    std::string name;
    CHECK(s.readName(1, name) && name == full);
    CHECK(s.readType(1) == 3);
    CHECK(kindFromType(3) == Kind::Mouse && kindFromType(0) == Kind::Keyboard);
    CHECK(kindFromType(8) == Kind::Headset && kindFromType(12) == Kind::Controller);
    CHECK(kindFromType(99) == Kind::Other);
}

static void test_notifications_ignored() {
    Fake f;
    f.handler = [](const Packet& r, std::deque<Packet>& q) {
        Packet note(20, 0);  // unrelated notification, swId 0
        note[0] = 0x11; note[1] = r[1]; note[2] = 0x06; note[3] = 0x00;
        q.push_back(note);
        Packet other = reply(r, {9, 9, 9});  // right header but for another device
        other[1] = static_cast<uint8_t>(r[1] + 1);
        q.push_back(other);
        Packet wrongSw = reply(r, {8, 8, 8});
        wrongSw[3] = static_cast<uint8_t>(r[3] ^ 0x01);
        q.push_back(wrongSw);
        q.push_back(Packet{0x20, 0, 0, 0, 0, 0, 0});  // DJ report: wrong id
        q.push_back(reply(r, {5, 0, 1}));
    };
    Session s(f.transport());
    Reply r = s.call(1, 0, 0, {0x10, 0x04});
    CHECK(r.ok && r.params[0] == 5);
}

int main() {
    test_feature_lookup();
    test_parsers();
    test_voltage();
    test_battery_via_session();
    test_errors();
    test_report_selection();
    test_name_assembly();
    test_notifications_ignored();
    std::puts("all hidpp tests passed");
    return 0;
}
