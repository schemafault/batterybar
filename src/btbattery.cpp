#include "device.h"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <initguid.h>  // must precede devpkey.h so the keys are defined here
#include <devpkey.h>

#include <cwctype>
#include <map>

// DEVPKEY_Bluetooth_Battery, pid 2, DEVPROP_TYPE_BYTE (percent).
DEFINE_DEVPROPKEY(kBluetoothBatteryKey, 0x104EA319, 0x6EE2, 0x4701, 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5, 2);

namespace {

bool GetProp(HDEVINFO set, SP_DEVINFO_DATA& dev, const DEVPROPKEY& key, DEVPROPTYPE want, void* buf, DWORD size) {
    DEVPROPTYPE type = 0;
    DWORD needed = 0;
    return SetupDiGetDevicePropertyW(set, &dev, &key, &type, static_cast<PBYTE>(buf), size, &needed, 0) && type == want;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool Has(const std::wstring& hay, const wchar_t* needle) { return hay.find(needle) != std::wstring::npos; }

DeviceKind Classify(const std::wstring& name) {
    std::wstring n = Lower(name);
    if (Has(n, L"headset") || Has(n, L"headphone") || Has(n, L"earbud") || Has(n, L"buds") || Has(n, L"airpods") ||
        Has(n, L"audio") || Has(n, L"wh-1000") || Has(n, L"speaker"))
        return DeviceKind::Headset;
    if (Has(n, L"controller") || Has(n, L"gamepad") || Has(n, L"xbox") || Has(n, L"dualsense") || Has(n, L"dualshock"))
        return DeviceKind::Controller;
    if (Has(n, L"mouse") || Has(n, L"mx master") || Has(n, L"anywhere") || Has(n, L"trackball")) return DeviceKind::Mouse;
    if (Has(n, L"keyboard") || Has(n, L"keys")) return DeviceKind::Keyboard;
    return DeviceKind::Other;
}

struct Node {
    std::wstring container;
    std::wstring name;
    int percent;
};

}  // namespace

std::vector<BatteryInfo> QueryBluetooth() {
    std::vector<BatteryInfo> out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return out;

    std::map<std::wstring, std::wstring> containerNames;  // container id -> device name
    std::vector<Node> withBattery;

    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA dev = {};
        dev.cbSize = sizeof(dev);
        if (!SetupDiEnumDeviceInfo(set, i, &dev)) break;

        wchar_t inst[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, inst, MAX_DEVICE_ID_LEN, nullptr)) continue;
        std::wstring id = Lower(inst);
        if (id.compare(0, 8, L"bthenum\\") != 0 && id.compare(0, 6, L"bthle\\") != 0) continue;

        GUID cid = {};
        wchar_t cidText[64] = {};
        if (!GetProp(set, dev, DEVPKEY_Device_ContainerId, DEVPROP_TYPE_GUID, &cid, sizeof(cid))) continue;
        wsprintfW(cidText, L"{%08lX-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X}", cid.Data1, cid.Data2, cid.Data3,
                  cid.Data4[0], cid.Data4[1], cid.Data4[2], cid.Data4[3], cid.Data4[4], cid.Data4[5], cid.Data4[6],
                  cid.Data4[7]);

        wchar_t name[256] = {};
        if (!GetProp(set, dev, DEVPKEY_Device_FriendlyName, DEVPROP_TYPE_STRING, name, sizeof(name)))
            GetProp(set, dev, DEVPKEY_Device_DeviceDesc, DEVPROP_TYPE_STRING, name, sizeof(name));

        // The DEV_ node carries the paired device's own name; profile nodes have generic ones.
        if (Has(id, L"\\dev_") && name[0]) containerNames[cidText] = name;

        BYTE level = 0;
        if (GetProp(set, dev, kBluetoothBatteryKey, DEVPROP_TYPE_BYTE, &level, sizeof(level)) && level <= 100)
            withBattery.push_back({cidText, name, level});
    }
    SetupDiDestroyDeviceInfoList(set);

    std::map<std::wstring, bool> seen;  // one entry per container
    for (const Node& n : withBattery) {
        if (seen[n.container]) continue;
        seen[n.container] = true;
        auto it = containerNames.find(n.container);
        BatteryInfo b;
        b.name = it != containerNames.end() ? it->second : n.name;
        if (b.name.empty()) b.name = L"Bluetooth device";
        b.kind = Classify(b.name);
        b.percent = n.percent;
        b.id = L"bt:" + n.container;
        out.push_back(b);
    }
    return out;
}
