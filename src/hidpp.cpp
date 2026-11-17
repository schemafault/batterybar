// Windows HID transport for Logitech HID++ 2.0 devices. The protocol logic
// lives in hidpp_proto.cpp; this file only enumerates, opens and does I/O.
#include "device.h"
#include "hidpp_proto.h"

#include <windows.h>
#include <setupapi.h>
// Some MinGW header versions lack extern "C" guards here; nesting is harmless elsewhere.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>
#include <cwctype>
#include <map>
#include <memory>

namespace {

constexpr USHORT kLogitechVid = 0x046D;
constexpr int kIoTimeoutMs = 500;

// One open HID collection (overlapped I/O). Report buffers always have the
// exact length the collection declares, as the HID stack requires.
struct Channel {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE event = nullptr;
    USHORT inLen = 0, outLen = 0;
    bool shortOk = false, longOk = false;
    std::wstring key;  // path without the collection suffix

    Channel() = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel() {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (event) CloseHandle(event);
    }

    // Waits for an overlapped operation. On timeout the I/O is cancelled and
    // we still wait for completion, so the buffer and OVERLAPPED stay valid.
    // Returns bytes transferred, 0 on timeout, -1 on error.
    int finish(OVERLAPPED& ov, BOOL started, int timeoutMs) {
        DWORD got = 0;
        if (!started) {
            if (GetLastError() != ERROR_IO_PENDING) return -1;
            if (WaitForSingleObject(event, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
                CancelIoEx(file, &ov);
                if (!GetOverlappedResult(file, &ov, &got, TRUE)) {
                    return GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
                }
                return static_cast<int>(got);  // completed just before the cancel
            }
        }
        if (!GetOverlappedResult(file, &ov, &got, FALSE)) return -1;
        return static_cast<int>(got);
    }

    bool write(const uint8_t* data, size_t n) {
        std::vector<uint8_t> buf(outLen, 0);
        std::copy(data, data + std::min<size_t>(n, buf.size()), buf.begin());
        OVERLAPPED ov = {};
        ov.hEvent = event;
        ResetEvent(event);
        BOOL started = WriteFile(file, buf.data(), outLen, nullptr, &ov);
        return finish(ov, started, kIoTimeoutMs) > 0;
    }

    int read(uint8_t* out, size_t cap, int timeoutMs) {
        std::vector<uint8_t> buf(inLen, 0);
        OVERLAPPED ov = {};
        ov.hEvent = event;
        ResetEvent(event);
        BOOL started = ReadFile(file, buf.data(), inLen, nullptr, &ov);
        int n = finish(ov, started, timeoutMs);
        if (n <= 0) return n;
        n = static_cast<int>(std::min<size_t>(static_cast<size_t>(n), cap));
        std::copy(buf.begin(), buf.begin() + n, out);
        return n;
    }
};

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// Opens a Logitech HID path and keeps it if it exposes a HID++ report collection.
std::unique_ptr<Channel> OpenChannel(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    auto ch = std::make_unique<Channel>();
    ch->file = h;  // closed by the destructor from here on

    HIDD_ATTRIBUTES attr = {};
    attr.Size = sizeof(attr);
    if (!HidD_GetAttributes(h, &attr) || attr.VendorID != kLogitechVid) return nullptr;

    PHIDP_PREPARSED_DATA pp = nullptr;
    if (!HidD_GetPreparsedData(h, &pp)) return nullptr;
    HIDP_CAPS caps = {};
    NTSTATUS st = HidP_GetCaps(pp, &caps);
    HidD_FreePreparsedData(pp);
    if (st != HIDP_STATUS_SUCCESS || caps.UsagePage < 0xFF00) return nullptr;  // 0xFF00 (receivers), 0xFF43 (BLE)

    const size_t inBytes = caps.InputReportByteLength, outBytes = caps.OutputReportByteLength;
    ch->shortOk = inBytes == hidpp::kShortLen && outBytes == hidpp::kShortLen;
    ch->longOk = inBytes == hidpp::kLongLen && outBytes == hidpp::kLongLen;
    if (!ch->shortOk && !ch->longOk) return nullptr;
    ch->inLen = caps.InputReportByteLength;
    ch->outLen = caps.OutputReportByteLength;
    ch->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ch->event) return nullptr;

    // Collections of one interface differ only by "&colNN"; strip it to group them.
    // The instance part also ends in a per-collection counter ("...&0&0001"), so drop that too:
    //   hid#vid_046d&pid_c52b&mi_02&col02#7&1ebf1dd&0&0001#{guid}  ->  hid#vid_046d&pid_c52b&mi_02#7&1ebf1dd&0
    ch->key = Lower(path);
    size_t pos = ch->key.find(L"&col");
    if (pos != std::wstring::npos) {
        size_t instStart = ch->key.find(L'#', pos);
        size_t instEnd = instStart == std::wstring::npos ? instStart : ch->key.find(L'#', instStart + 1);
        if (instEnd != std::wstring::npos) {
            std::wstring inst = ch->key.substr(instStart, instEnd - instStart);
            size_t amp = inst.rfind(L'&');
            if (amp != std::wstring::npos) inst.erase(amp);
            ch->key = ch->key.substr(0, pos) + inst;
        }
    }
    return ch;
}

DeviceKind ToKind(hidpp::Kind k) {
    switch (k) {
        case hidpp::Kind::Keyboard: return DeviceKind::Keyboard;
        case hidpp::Kind::Mouse: return DeviceKind::Mouse;
        case hidpp::Kind::Headset: return DeviceKind::Headset;
        case hidpp::Kind::Controller: return DeviceKind::Controller;
        default: return DeviceKind::Other;
    }
}

void ProbeChannel(Channel& ch, std::vector<BatteryInfo>& out) {
    hidpp::Transport t;
    t.canShort = ch.shortOk;
    t.canLong = ch.longOk;
    t.write = [&ch](const uint8_t* d, size_t n) { return ch.write(d, n); };
    t.read = [&ch](uint8_t* b, size_t cap, int ms) { return ch.read(b, cap, ms); };
    hidpp::Session session(t, kIoTimeoutMs);

    auto add = [&](uint8_t idx, const hidpp::DeviceInfo& info) {
        BatteryInfo b;
        b.name = FromUtf8(info.name);
        if (b.name.empty()) b.name = L"Logitech device";
        b.kind = ToKind(info.kind);
        b.percent = info.battery.percent;
        b.charging = info.battery.charging;
        b.approximate = info.battery.approximate;
        b.id = L"hidpp:" + ch.key + L":" + std::to_wstring(idx);
        out.push_back(b);
    };

    hidpp::DeviceInfo info;
    if (session.probe(hidpp::kDirectIndex, info)) {  // direct Bluetooth or USB device
        add(hidpp::kDirectIndex, info);
        return;
    }
    for (uint8_t idx = 1; idx <= 6; ++idx) {  // receiver slots
        hidpp::DeviceInfo d;
        if (session.probe(idx, d)) add(idx, d);
    }
}

}  // namespace

std::vector<BatteryInfo> QueryHidpp() {
    std::vector<BatteryInfo> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    // Group collections per interface; prefer the long-report collection since
    // many devices only answer long requests and reply in kind.
    std::map<std::wstring, std::unique_ptr<Channel>> chosen;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifd = {};
        ifd.cbSize = sizeof(ifd);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (need == 0) continue;
        std::vector<BYTE> raw(need, 0);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;

        std::wstring path = detail->DevicePath;
        if (Lower(path).find(L"vid_046d") == std::wstring::npos) continue;  // cheap filter before opening
        std::unique_ptr<Channel> ch = OpenChannel(path);
        if (!ch) continue;
        auto it = chosen.find(ch->key);
        if (it == chosen.end()) chosen[ch->key] = std::move(ch);
        else if (!it->second->longOk && ch->longOk) it->second = std::move(ch);
    }
    SetupDiDestroyDeviceInfoList(set);

    for (auto& kv : chosen) ProbeChannel(*kv.second, out);

    // Drop duplicates (same device reachable through more than one interface).
    std::vector<BatteryInfo> unique;
    for (auto& b : out) {
        bool dup = std::any_of(unique.begin(), unique.end(), [&](const BatteryInfo& u) {
            return Lower(u.name) == Lower(b.name) && u.kind == b.kind;
        });
        if (!dup) unique.push_back(b);
    }
    return unique;
}
