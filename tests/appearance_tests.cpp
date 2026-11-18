// Plain assert-based tests for the button appearance rules (colours, text,
// label and the rendered icon backdrop). No Windows APIs involved.
#include "../src/appearance.h"

#include <cstdio>
#include <cstdlib>

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

static const uint32_t kGrey = 0xAAAAAA, kGreen = 0x50DC64, kAmber = 0xFFC107, kRed = 0xFF5252;

static void test_colour_thresholds() {
    CHECK(Describe(Make(100)).textColour == kGreen);
    CHECK(Describe(Make(51)).textColour == kGreen);
    CHECK(Describe(Make(50)).textColour == kAmber);
    CHECK(Describe(Make(21)).textColour == kAmber);
    CHECK(Describe(Make(20)).textColour == kRed);
    CHECK(Describe(Make(0)).textColour == kRed);
}

static void test_unknown() {
    BatteryInfo b = Make(-1);
    ButtonAppearance a = Describe(b);
    CHECK(a.text == L"?");
    CHECK(a.textColour == kGrey);
    CHECK(a.label == L"MX Master 3S");
}

static void test_stale_text_is_grey() {
    BatteryInfo b = Make(90);
    b.stale = true;
    CHECK(Describe(b).textColour == kGrey);
    CHECK(Describe(b).text == L"90");
}

static void test_text() {
    CHECK(Describe(Make(87)).text == L"87");
    CHECK(Describe(Make(0)).text == L"0");
    CHECK(Describe(Make(100)).text == L"100");
    CHECK(Describe(Make(130)).text == L"100");
}

static void test_glyph_colours() {
    BatteryInfo b = Make(50);
    CHECK(Describe(b).glyphColour == 0xF0F0F0);
    CHECK(Describe(b).backColour == 0x202020);
    b.charging = true;
    CHECK(Describe(b).glyphColour == 0xFFD54F);
    b.stale = true;  // asleep wins over charging
    CHECK(Describe(b).glyphColour == 0x8A8A8A);
    b.charging = false;
    CHECK(Describe(b).glyphColour == 0x8A8A8A);
}

static void test_label_variants() {
    BatteryInfo b = Make(87);
    CHECK(Describe(b).label == L"MX Master 3S: 87%");
    b.approximate = true;
    CHECK(Describe(b).label == L"MX Master 3S: ~87%");
    b.approximate = false;
    b.charging = true;
    CHECK(Describe(b).label == L"MX Master 3S: 87% (charging)");
    b.charging = false;
    b.stale = true;
    CHECK(Describe(b).label == L"MX Master 3S: 87% (asleep, last reading)");
    b.charging = true;
    CHECK(Describe(b).label == L"MX Master 3S: 87% (charging) (asleep, last reading)");
}

static void test_text_band() {
    TextBand t = TextBandFor(32);
    CHECK(t.top == 32 * 46 / 100);
    CHECK(t.bottom == 32 * 98 / 100);
    TextBand t64 = TextBandFor(64);
    CHECK(t64.top == 29 && t64.bottom == 62);
}

static uint32_t At(const std::vector<uint32_t>& px, int size, int x, int y) {
    return px[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
}

static void test_backdrop_shape() {
    for (int size : {16, 32, 64}) {
        std::vector<uint32_t> px = RenderBackdrop(Make(50), size);
        CHECK(px.size() == static_cast<size_t>(size) * static_cast<size_t>(size));
        // At 16px the corner pixel is still faintly covered by the curve, so
        // only require it to be mostly transparent there.
        const int corners[4][2] = {{0, 0}, {size - 1, 0}, {0, size - 1}, {size - 1, size - 1}};
        for (const auto& c : corners) {
            uint32_t p = At(px, size, c[0], c[1]);
            if (size >= 32) CHECK(p == 0);
            else CHECK((p >> 24) < 0x40);
        }
        CHECK(At(px, size, size / 2, size / 2) == 0xFF202020u);
    }
}

static void test_backdrop_text_band_is_plain() {
    for (DeviceKind k : kAllKinds)
        for (int size : {16, 32, 64}) {
            std::vector<uint32_t> px = RenderBackdrop(Make(50, k), size);
            TextBand t = TextBandFor(size);
            for (int y = t.top; y < t.bottom; ++y)
                for (int x = 0; x < size; ++x) {
                    uint32_t p = At(px, size, x, y);
                    // Either outside the rounded square or exactly the back colour.
                    CHECK(p == 0 || (p & 0x00FFFFFFu) == 0x202020u);
                }
        }
}

static int GlyphPixels(const std::vector<uint32_t>& px, int size, uint32_t glyph) {
    int n = 0;
    for (int y = 0; y < size / 2; ++y)
        for (int x = 0; x < size; ++x)
            if ((At(px, size, x, y) & 0x00FFFFFFu) == glyph) ++n;
    return n;
}

static int GlyphColumns(const std::vector<uint32_t>& px, int size, uint32_t glyph) {
    int cols = 0;
    for (int x = 0; x < size; ++x)
        for (int y = 0; y < size; ++y)
            if ((At(px, size, x, y) & 0x00FFFFFFu) == glyph) {
                ++cols;
                break;
            }
    return cols;
}

static void test_every_kind_draws_a_glyph() {
    for (DeviceKind k : kAllKinds)
        for (int size : {16, 32, 64})
            CHECK(GlyphPixels(RenderBackdrop(Make(50, k), size), size, 0xF0F0F0) > 0);
}

static void test_glyph_follows_colour() {
    BatteryInfo b = Make(50, DeviceKind::Keyboard);
    b.charging = true;
    CHECK(GlyphPixels(RenderBackdrop(b, 64), 64, 0xFFD54F) > 0);
    CHECK(GlyphPixels(RenderBackdrop(b, 64), 64, 0xF0F0F0) == 0);
    b.stale = true;
    CHECK(GlyphPixels(RenderBackdrop(b, 64), 64, 0x8A8A8A) > 0);
}

static void test_kinds_differ() {
    CHECK(RenderBackdrop(Make(50, DeviceKind::Mouse), 32) != RenderBackdrop(Make(50, DeviceKind::Keyboard), 32));
    CHECK(RenderBackdrop(Make(50, DeviceKind::Headset), 32) != RenderBackdrop(Make(50, DeviceKind::Controller), 32));
}

static void test_keyboard_wider_than_mouse() {
    for (int size : {32, 64}) {
        int mouse = GlyphColumns(RenderBackdrop(Make(50, DeviceKind::Mouse), size), size, 0xF0F0F0);
        int kb = GlyphColumns(RenderBackdrop(Make(50, DeviceKind::Keyboard), size), size, 0xF0F0F0);
        CHECK(mouse > 0);
        CHECK(kb > mouse);
    }
}

static void test_kind_letter() {
    CHECK(KindLetter(DeviceKind::Mouse) == L'M');
    CHECK(KindLetter(DeviceKind::Keyboard) == L'K');
    CHECK(KindLetter(DeviceKind::Headset) == L'H');
    CHECK(KindLetter(DeviceKind::Controller) == L'C');
    CHECK(KindLetter(DeviceKind::Other) == L'?');
}

int main() {
    test_colour_thresholds();
    test_unknown();
    test_stale_text_is_grey();
    test_text();
    test_glyph_colours();
    test_label_variants();
    test_text_band();
    test_backdrop_shape();
    test_backdrop_text_band_is_plain();
    test_every_kind_draws_a_glyph();
    test_glyph_follows_colour();
    test_kinds_differ();
    test_keyboard_wider_than_mouse();
    test_kind_letter();
    std::puts("all appearance tests passed");
    return 0;
}
