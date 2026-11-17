#include "device.h"

#include <windows.h>

namespace {

// Declared here so we do not depend on the SDK's xinput.h or import library.
struct XInputBatteryInfo {
    BYTE type;
    BYTE level;
};
constexpr BYTE kTypeAlkaline = 0x02;
constexpr BYTE kTypeNiMh = 0x03;
constexpr BYTE kDevTypeGamepad = 0x00;
using GetBatteryFn = DWORD(WINAPI*)(DWORD, BYTE, XInputBatteryInfo*);

}  // namespace

std::vector<BatteryInfo> QueryXInput() {
    std::vector<BatteryInfo> out;
    // xinput1_4 (Windows 8+) has battery support; xinput9_1_0 does not.
    HMODULE lib = LoadLibraryW(L"xinput1_4.dll");
    if (!lib) return out;
    auto getBattery = reinterpret_cast<GetBatteryFn>(reinterpret_cast<void*>(GetProcAddress(lib, "XInputGetBatteryInformation")));
    if (getBattery) {
        static const int kLevelPercent[] = {5, 30, 65, 100};  // empty, low, medium, full
        for (DWORD i = 0; i < 4; ++i) {
            XInputBatteryInfo bi = {};
            if (getBattery(i, kDevTypeGamepad, &bi) != ERROR_SUCCESS) continue;
            if (bi.type != kTypeAlkaline && bi.type != kTypeNiMh) continue;  // skip wired, disconnected, unknown
            if (bi.level > 3) continue;
            BatteryInfo b;
            b.name = L"Controller " + std::to_wstring(i + 1);
            b.kind = DeviceKind::Controller;
            b.percent = kLevelPercent[bi.level];
            b.approximate = true;
            b.id = L"xinput:" + std::to_wstring(i);
            out.push_back(b);
        }
    }
    FreeLibrary(lib);
    return out;
}
