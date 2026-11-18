#include "roster.h"

#include <algorithm>
#include <cwctype>

namespace {

// How long a silent HID++ device stays on screen as stale.
constexpr uint64_t STALE_KEEP_MS = 30ull * 60 * 1000;

std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
    return s;
}

// Names are free text from different APIs, so match on containment. Very short
// names would match almost anything, so they never overlap.
bool NamesOverlap(const std::wstring& a, const std::wstring& b) {
    std::wstring x = LowerCopy(a), y = LowerCopy(b);
    if (x.size() < 3 || y.size() < 3) return false;
    return x.find(y) != std::wstring::npos || y.find(x) != std::wstring::npos;
}

}  // namespace

std::vector<BatteryInfo> Roster::update(const ProviderReadings& readings, uint64_t nowMs) {
    // One device can be reported twice by HID++, so keep the first of each name and kind.
    std::vector<BatteryInfo> hid;
    for (const BatteryInfo& b : readings.hidpp) {
        const std::wstring name = LowerCopy(b.name);
        bool dup = std::any_of(hid.begin(), hid.end(), [&](const BatteryInfo& h) {
            return h.kind == b.kind && LowerCopy(h.name) == name;
        });
        if (!dup) hid.push_back(b);
    }
    std::vector<BatteryInfo> all = hid;

    bool btController = false;
    for (const BatteryInfo& b : readings.bluetooth) {
        bool dup = std::any_of(hid.begin(), hid.end(),
                               [&](const BatteryInfo& h) { return NamesOverlap(h.name, b.name); });
        if (dup) continue;
        btController = btController || b.kind == DeviceKind::Controller;
        all.push_back(b);
    }
    // XInput slots cannot be matched to Bluetooth devices, so skip them if a
    // Bluetooth controller was already reported.
    if (!btController) all.insert(all.end(), readings.xinput.begin(), readings.xinput.end());

    // Only HID++ devices go quiet while still present, so only they are kept.
    for (const BatteryInfo& b : hid) seen_[b.id] = {b, nowMs};
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (nowMs - it->second.second > STALE_KEEP_MS) {
            it = seen_.erase(it);
            continue;
        }
        const BatteryInfo& old = it->second.first;
        bool present = std::any_of(all.begin(), all.end(), [&](const BatteryInfo& b) {
            return b.id == old.id || (b.kind == old.kind && NamesOverlap(b.name, old.name));
        });
        if (!present) {
            BatteryInfo b = old;
            b.stale = true;
            b.charging = false;
            all.push_back(b);
        }
        ++it;
    }

    std::stable_sort(all.begin(), all.end(), [](const BatteryInfo& a, const BatteryInfo& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
    return all;
}
