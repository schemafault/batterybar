// Plain assert-based tests for the button appearance rules (colours, text,
// label, picture and layout). No Windows APIs involved.
#include "../src/appearance.h"

#include <cstdio>
#include <cstdlib>
#include <set>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

static const DeviceKind kAllKinds[] = {DeviceKind::Mouse, DeviceKind::Keyboard, DeviceKind::Headset,
                                       DeviceKind::Controller, DeviceKind::Other};

static BatteryInfo Make(int percent, DeviceKind kind = DeviceKind::Mouse) {
    BatteryInfo b;
    b.name = L"MX Master 3S";
    b.kind = kind;
    b.percent = percent;
    return b;
}

// On a dark taskbar / on a light taskbar.
static const uint32_t kWhite = 0xFFFFFF, kRed = 0xFF6B6B, kGreen = 0x6CCB5F;
static const uint32_t kInk = 0x1A1A1A, kDarkRed = 0xC42B1C, kDarkGreen = 0x0F7B0F;

static void test_number_is_plain_until_low() {
    CHECK(Describe(Make(100)).textColour == kWhite);
    CHECK(Describe(Make(50)).textColour == kWhite);
    CHECK(Describe(Make(21)).textColour == kWhite);
    CHECK(Describe(Make(20)).textColour == kRed);
    CHECK(Describe(Make(0)).textColour == kRed);
}

static void test_charging_is_green_even_when_low() {
    BatteryInfo b = Make(80);
    b.charging = true;
    CHECK(Describe(b).textColour == kGreen);
    b.percent = 5;
    CHECK(Describe(b).textColour == kGreen);
}

static void test_light_taskbar_uses_dark_colours() {
    CHECK(Describe(Make(80), true).textColour == kInk);
    CHECK(Describe(Make(80), true).glyphColour == kInk);
    CHECK(Describe(Make(10), true).textColour == kDarkRed);
    BatteryInfo b = Make(80);
    b.charging = true;
    CHECK(Describe(b, true).textColour == kDarkGreen);
}

static void test_picture_never_takes_a_state_colour() {
    BatteryInfo b = Make(10);
    CHECK(Describe(b).glyphColour == kWhite);
    b.charging = true;
    CHECK(Describe(b).glyphColour == kWhite);
}

static void test_unknown() {
    ButtonAppearance a = Describe(Make(-1));
    CHECK(a.text == L"?");
    CHECK(a.textColour == kWhite);
    CHECK(a.label == L"MX Master 3S");
}

static void test_asleep_is_faded_and_plain() {
    CHECK(Describe(Make(90)).opacity == 255);
    BatteryInfo b = Make(10);
    b.stale = true;
    CHECK(Describe(b).opacity < 255);
    CHECK(Describe(b).opacity > 0);
    CHECK(Describe(b).textColour == kWhite);  // a last reading is not an alarm
    CHECK(Describe(b).text == L"10");
    b.charging = true;  // asleep wins over charging
    CHECK(Describe(b).textColour == kWhite);
}

static void test_text() {
    CHECK(Describe(Make(87)).text == L"87");
    CHECK(Describe(Make(0)).text == L"0");
    CHECK(Describe(Make(100)).text == L"100");
    CHECK(Describe(Make(130)).text == L"100");
}

static void test_every_kind_has_its_own_picture() {
    std::set<wchar_t> seen;
    for (DeviceKind k : kAllKinds) {
        wchar_t g = Describe(Make(50, k)).glyph;
        CHECK(g >= 0xE700 && g <= 0xF8FF);  // the icon fonts' private-use range
        CHECK(seen.insert(g).second);
    }
}

static void test_label_variants() {
    BatteryInfo b = Make(87);
    CHECK(Describe(b).label == L"87% - MX Master 3S");
    b.approximate = true;
    CHECK(Describe(b).label == L"~87% - MX Master 3S");
    b.approximate = false;
    b.charging = true;
    CHECK(Describe(b).label == L"87% (charging) - MX Master 3S");
    b.charging = false;
    b.stale = true;
    CHECK(Describe(b).label == L"87% (asleep, last reading) - MX Master 3S");
    b.charging = true;
    CHECK(Describe(b).label == L"87% (charging) (asleep, last reading) - MX Master 3S");
}

static void test_layout_keeps_picture_and_number_apart() {
    for (int size : {16, 24, 32, 48, 64}) {
        IconLayout at = LayoutFor(size);
        CHECK(at.glyphEm > 0 && at.textEm > 0);
        CHECK(at.glyphEm <= at.glyphBottom);     // the picture fits its band
        CHECK(at.textBaseline <= size);
        // Digits rise about three quarters of the font size above the baseline.
        CHECK(at.textBaseline - at.textEm * 3 / 4 >= at.glyphBottom);
    }
}

static void test_ring_styles_show_two_things_each() {
    for (int size : {16, 24, 32, 64}) {
        IconLayout picture = LayoutFor(size, IconStyle::PictureInRing);
        CHECK(picture.ring && picture.glyphEm > 0 && picture.textEm == 0);
        CHECK(picture.glyphBottom == size);  // centred in the whole icon
        IconLayout number = LayoutFor(size, IconStyle::NumberInRing);
        CHECK(number.ring && number.glyphEm == 0 && number.textEm > 0);
        CHECK(number.textBaseline < 0);  // centred, not on a baseline
        // Only three digits shrink, so 5 and 87 are the same size.
        CHECK(LayoutFor(size, IconStyle::NumberInRing, 1).textEm == number.textEm);
        CHECK(LayoutFor(size, IconStyle::NumberInRing, 3).textEm < number.textEm);
        CHECK(!LayoutFor(size, IconStyle::PictureAndNumber).ring);
        CHECK(LayoutFor(size, IconStyle::PictureAndNumber, 3).textEm == LayoutFor(size).textEm);
    }
}

static void test_style_names_differ() {
    std::set<std::wstring> names;
    for (int i = 0; i < kIconStyleCount; ++i) CHECK(names.insert(StyleName(static_cast<IconStyle>(i))).second);
}

static int Sum(const std::vector<uint8_t>& v) {
    int n = 0;
    for (uint8_t c : v) n += c;
    return n;
}

static void test_ring_fills_clockwise_from_the_top() {
    for (int size : {24, 64}) {
        const size_t count = static_cast<size_t>(size) * static_cast<size_t>(size);
        auto at = [size](int x, int y) { return static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x); };
        const int mid = size / 2, edge = size * 6 / 100;  // a pixel inside the band on each side

        RingMask full = RenderRing(size, 100), empty = RenderRing(size, 0), half = RenderRing(size, 50);
        CHECK(full.track.size() == count && full.level.size() == count);
        CHECK(full.level == full.track);
        CHECK(Sum(empty.level) == 0);
        CHECK(empty.track == full.track);  // the faint circle never changes

        // The middle and the corners are left for the picture or number.
        CHECK(full.track[at(mid, mid)] == 0);
        CHECK(full.track[at(0, 0)] == 0 && full.track[at(size - 1, size - 1)] == 0);
        CHECK(full.track[at(mid, edge)] == 255);

        // At 50% the right side is filled and the left is not.
        CHECK(half.level[at(size - 1 - edge, mid - 2)] == 255);
        CHECK(half.level[at(edge, mid - 2)] == 0);
        // A quarter fills the top right only.
        RingMask quarter = RenderRing(size, 25);
        CHECK(quarter.level[at(size - 1 - edge, mid + size / 8)] == 0);

        int last = 0;  // more battery never means less ring
        for (int p = 0; p <= 100; p += 5) {
            RingMask m = RenderRing(size, p);
            for (size_t i = 0; i < count; ++i) CHECK(m.level[i] <= m.track[i]);
            CHECK(Sum(m.level) >= last);
            last = Sum(m.level);
        }
        CHECK(RenderRing(size, 130).level == full.level);
        CHECK(RenderRing(size, -1).level == empty.level);
    }
    CHECK(RenderRing(0, 50).track.empty());
}

static void test_level_follows_percent() {
    CHECK(Describe(Make(87)).level == 87);
    CHECK(Describe(Make(130)).level == 100);
    CHECK(Describe(Make(-1)).level == 0);
}

static void test_kind_letter() {
    CHECK(KindLetter(DeviceKind::Mouse) == L'M');
    CHECK(KindLetter(DeviceKind::Keyboard) == L'K');
    CHECK(KindLetter(DeviceKind::Headset) == L'H');
    CHECK(KindLetter(DeviceKind::Controller) == L'C');
    CHECK(KindLetter(DeviceKind::Other) == L'?');
}

int main() {
    test_number_is_plain_until_low();
    test_charging_is_green_even_when_low();
    test_light_taskbar_uses_dark_colours();
    test_picture_never_takes_a_state_colour();
    test_unknown();
    test_asleep_is_faded_and_plain();
    test_text();
    test_every_kind_has_its_own_picture();
    test_label_variants();
    test_layout_keeps_picture_and_number_apart();
    test_ring_styles_show_two_things_each();
    test_style_names_differ();
    test_ring_fills_clockwise_from_the_top();
    test_level_follows_percent();
    test_kind_letter();
    std::puts("all appearance tests passed");
    return 0;
}
