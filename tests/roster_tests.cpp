// Plain assert-based tests for the device roster merge rules.
#include "../src/roster.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static const uint64_t MIN = 60 * 1000;

static BatteryInfo dev(const wchar_t* name, DeviceKind kind, const wchar_t* id, int percent = 50) {
    BatteryInfo b;
    b.name = name;
    b.kind = kind;
    b.id = id;
    b.percent = percent;
    return b;
}

static const BatteryInfo* find(const std::vector<BatteryInfo>& v, const wchar_t* id) {
    for (const BatteryInfo& b : v)
        if (b.id == id) return &b;
    return nullptr;
}

static void hidppBeatsBluetooth() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1")};
    in.bluetooth = {dev(L"mx master 3 mouse", DeviceKind::Mouse, L"bt:1")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 1);
    CHECK(out[0].id == L"hidpp:1");
}

static void unrelatedBluetoothKept() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1")};
    in.bluetooth = {dev(L"WH-1000XM4", DeviceKind::Headset, L"bt:1")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 2);
    CHECK(find(out, L"bt:1") != nullptr);
}

static void shortNamesNeverOverlap() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX", DeviceKind::Mouse, L"hidpp:1")};
    in.bluetooth = {dev(L"MX", DeviceKind::Mouse, L"bt:1"), dev(L"MX Master", DeviceKind::Mouse, L"bt:2")};
    CHECK(r.update(in, 0).size() == 3);
    // The other side being short counts too.
    Roster r2;
    ProviderReadings in2;
    in2.hidpp = {dev(L"MX Master", DeviceKind::Mouse, L"hidpp:1")};
    in2.bluetooth = {dev(L"MX", DeviceKind::Mouse, L"bt:1")};
    CHECK(r2.update(in2, 0).size() == 2);
}

static void bluetoothControllerSuppressesXInput() {
    Roster r;
    ProviderReadings in;
    in.bluetooth = {dev(L"Wireless Controller", DeviceKind::Controller, L"bt:1")};
    in.xinput = {dev(L"Xbox 1", DeviceKind::Controller, L"xinput:0")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 1);
    CHECK(out[0].id == L"bt:1");
}

static void xinputKeptOtherwise() {
    Roster r;
    ProviderReadings in;
    in.bluetooth = {dev(L"WH-1000XM4", DeviceKind::Headset, L"bt:1")};
    in.xinput = {dev(L"Xbox 1", DeviceKind::Controller, L"xinput:0")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 2);
    CHECK(find(out, L"xinput:0") != nullptr);
}

static void droppedBluetoothControllerDoesNotSuppressXInput() {
    // The controller is a duplicate of a HID++ device, so it never counts.
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"Logitech F710", DeviceKind::Controller, L"hidpp:1")};
    in.bluetooth = {dev(L"Logitech F710", DeviceKind::Controller, L"bt:1")};
    in.xinput = {dev(L"Xbox 1", DeviceKind::Controller, L"xinput:0")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 2);
    CHECK(find(out, L"xinput:0") != nullptr);
}

static void hidppDuplicates() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1", 80),
                dev(L"mx master 3", DeviceKind::Mouse, L"hidpp:2", 20),
                dev(L"MX Master 3", DeviceKind::Keyboard, L"hidpp:3")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 2);
    CHECK(find(out, L"hidpp:1") != nullptr);  // first wins
    CHECK(find(out, L"hidpp:2") == nullptr);
    CHECK(find(out, L"hidpp:3") != nullptr);  // different kind kept
}

static void hidppGoesStale() {
    Roster r;
    ProviderReadings in;
    BatteryInfo m = dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1", 70);
    m.charging = true;
    in.hidpp = {m};
    auto out = r.update(in, 1000);
    CHECK(out.size() == 1 && !out[0].stale && out[0].charging);

    out = r.update(ProviderReadings{}, 1000 + MIN);
    CHECK(out.size() == 1);
    CHECK(out[0].id == L"hidpp:1");
    CHECK(out[0].stale);
    CHECK(!out[0].charging);
    CHECK(out[0].percent == 70);
}

static void staleExpiry() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1")};
    r.update(in, 5000);
    const uint64_t limit = 30 * MIN;
    CHECK(r.update(ProviderReadings{}, 5000 + limit - 1).size() == 1);
    CHECK(r.update(ProviderReadings{}, 5000 + limit).size() == 1);  // forgotten only when greater
    CHECK(r.update(ProviderReadings{}, 5000 + limit + 1).empty());
    // Once forgotten it stays gone.
    CHECK(r.update(ProviderReadings{}, 5000 + limit + 2).empty());
}

static void staleTimerRunsFromLastSeen() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1")};
    r.update(in, 0);
    r.update(in, 20 * MIN);  // seen again, so the timer restarts
    CHECK(r.update(ProviderReadings{}, 49 * MIN).size() == 1);
    CHECK(r.update(ProviderReadings{}, 51 * MIN).empty());
}

static void nonHidppNotKeptStale() {
    Roster r;
    ProviderReadings in;
    in.bluetooth = {dev(L"WH-1000XM4", DeviceKind::Headset, L"bt:1")};
    in.xinput = {dev(L"Xbox 1", DeviceKind::Controller, L"xinput:0")};
    CHECK(r.update(in, 0).size() == 2);
    CHECK(r.update(ProviderReadings{}, MIN).empty());
}

static void idPrefixIsIrrelevant() {
    // Staleness depends on the provider, not on what the id looks like.
    Roster r;
    ProviderReadings in;
    in.bluetooth = {dev(L"Some Mouse", DeviceKind::Mouse, L"hidpp:fake")};
    r.update(in, 0);
    CHECK(r.update(ProviderReadings{}, MIN).empty());
    ProviderReadings in2;
    in2.hidpp = {dev(L"Other Mouse", DeviceKind::Mouse, L"weird-id")};
    r.update(in2, 2 * MIN);
    CHECK(r.update(ProviderReadings{}, 3 * MIN).size() == 1);
}

static void staleNotDuplicatedUnderNewId() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:old")};
    r.update(in, 0);
    ProviderReadings back;
    back.hidpp = {dev(L"MX Master 3 for Mac", DeviceKind::Mouse, L"hidpp:new")};
    auto out = r.update(back, MIN);
    CHECK(out.size() == 1);
    CHECK(out[0].id == L"hidpp:new");
    CHECK(!out[0].stale);
}

static void staleKeptForDifferentKind() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"MX Master 3", DeviceKind::Mouse, L"hidpp:1")};
    r.update(in, 0);
    ProviderReadings other;
    other.bluetooth = {dev(L"MX Master 3 Keys", DeviceKind::Keyboard, L"bt:1")};
    auto out = r.update(other, MIN);
    CHECK(out.size() == 2);
    CHECK(find(out, L"hidpp:1") != nullptr && find(out, L"hidpp:1")->stale);
}

static void sortOrder() {
    Roster r;
    ProviderReadings in;
    in.hidpp = {dev(L"Zed Keys", DeviceKind::Keyboard, L"hidpp:1"),
                dev(L"Beta Mouse", DeviceKind::Mouse, L"hidpp:2"),
                dev(L"Alpha Mouse", DeviceKind::Mouse, L"hidpp:3")};
    in.bluetooth = {dev(L"Cans", DeviceKind::Headset, L"bt:1")};
    in.xinput = {dev(L"Pad", DeviceKind::Controller, L"xinput:0")};
    auto out = r.update(in, 0);
    CHECK(out.size() == 5);
    CHECK(out[0].id == L"hidpp:3");
    CHECK(out[1].id == L"hidpp:2");
    CHECK(out[2].id == L"hidpp:1");
    CHECK(out[3].id == L"bt:1");
    CHECK(out[4].id == L"xinput:0");
    // Stale entries are sorted in with the rest.
    ProviderReadings later;
    later.hidpp = {dev(L"Zed Keys", DeviceKind::Keyboard, L"hidpp:1")};
    out = r.update(later, MIN);
    CHECK(out.size() == 3);
    CHECK(out[0].id == L"hidpp:3" && out[0].stale);
    CHECK(out[1].id == L"hidpp:2" && out[1].stale);
    CHECK(out[2].id == L"hidpp:1" && !out[2].stale);
}

int main() {
    hidppBeatsBluetooth();
    unrelatedBluetoothKept();
    shortNamesNeverOverlap();
    bluetoothControllerSuppressesXInput();
    xinputKeptOtherwise();
    droppedBluetoothControllerDoesNotSuppressXInput();
    hidppDuplicates();
    hidppGoesStale();
    staleExpiry();
    staleTimerRunsFromLastSeen();
    nonHidppNotKeptStale();
    idPrefixIsIrrelevant();
    staleNotDuplicatedUnderNewId();
    staleKeptForDifferentKind();
    sortOrder();
    std::puts("all roster tests passed");
    return 0;
}
