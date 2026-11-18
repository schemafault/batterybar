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
constexpr int kIoTimeoutMs = 1000;

// One open HID collection (overlapped I/O). Report buffers always have the
// exact length the collection declares, as the HID stack requires.
struct Pipe {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE readEvent = nullptr, writeEvent = nullptr;
    USHORT inLen = 0, outLen = 0;
    OVERLAPPED ov = {};          // the read in flight; must not move while pending
    std::vector<uint8_t> buf;
    bool pending = false;

    Pipe() = default;
    Pipe(const Pipe&) = delete;
    Pipe& operator=(const Pipe&) = delete;
    ~Pipe() {
        if (pending) {  // wait for the cancel so the kernel is done with ov and buf
            DWORD got = 0;
            CancelIoEx(file, &ov);
            GetOverlappedResult(file, &ov, &got, TRUE);
        }
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        if (readEvent) CloseHandle(readEvent);
        if (writeEvent) CloseHandle(writeEvent);
    }

    // Leaves one read in flight. It stays pending across calls, so a reply
    // that arrives late is still picked up by the next read.
    void startRead() {
        if (pending) return;
        buf.assign(inLen, 0);
        ov = OVERLAPPED{};
        ov.hEvent = readEvent;
        ResetEvent(readEvent);
        if (ReadFile(file, buf.data(), inLen, nullptr, &ov) || GetLastError() == ERROR_IO_PENDING) pending = true;
    }

    bool write(const uint8_t* data, size_t n) {
        std::vector<uint8_t> out(outLen, 0);
        std::copy(data, data + std::min<size_t>(n, out.size()), out.begin());
        OVERLAPPED wov = {};
        wov.hEvent = writeEvent;
        ResetEvent(writeEvent);
        DWORD wrote = 0;
        if (!WriteFile(file, out.data(), outLen, nullptr, &wov)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (WaitForSingleObject(writeEvent, kIoTimeoutMs) != WAIT_OBJECT_0) CancelIoEx(file, &wov);
            if (!GetOverlappedResult(file, &wov, &wrote, TRUE)) return false;
            return wrote > 0;
        }
        return GetOverlappedResult(file, &wov, &wrote, FALSE) && wrote > 0;
    }
};

// The HID++ collections of one interface. Windows splits short (7-byte) and
// long (20-byte) reports into separate collections, and a reply can arrive on
// either whatever the request used: devices answer in long reports, while a
// receiver reports an empty slot in a short one. So both are read together.
struct Channel {
    std::unique_ptr<Pipe> shortPipe, longPipe;
    std::wstring key;  // path without the collection suffix

    bool write(const uint8_t* data, size_t n) {
        if (n == 0) return false;
        Pipe* p = data[0] == hidpp::kReportShort ? shortPipe.get() : longPipe.get();
        return p && p->write(data, n);
    }

    // Returns bytes read from whichever collection answers first, 0 on timeout, -1 on error.
    int read(uint8_t* out, size_t cap, int timeoutMs) {
        Pipe* pipes[2] = {};
        HANDLE events[2] = {};
        DWORD count = 0;
        for (Pipe* p : {shortPipe.get(), longPipe.get()}) {
            if (!p) continue;
            p->startRead();
            if (!p->pending) continue;
            pipes[count] = p;
            events[count++] = p->readEvent;
        }
        if (count == 0) return -1;
        DWORD w = WaitForMultipleObjects(count, events, FALSE, static_cast<DWORD>(timeoutMs));
        if (w == WAIT_TIMEOUT) return 0;
        if (w >= WAIT_OBJECT_0 + count) return -1;
        Pipe& p = *pipes[w - WAIT_OBJECT_0];
        DWORD got = 0;
        BOOL ok = GetOverlappedResult(p.file, &p.ov, &got, FALSE);
        p.pending = false;
        if (!ok) return -1;
        size_t n = std::min<size_t>(got, cap);
        std::copy(p.buf.begin(), p.buf.begin() + n, out);
        return static_cast<int>(n);
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

// Opens a Logitech HID path and keeps it if it is a HID++ report collection.
// isLong says which report size it carries; key identifies its interface.
std::unique_ptr<Pipe> OpenPipe(const std::wstring& path, bool& isLong, std::wstring& key) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    auto p = std::make_unique<Pipe>();
    p->file = h;  // closed by the destructor from here on

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
    const bool shortOk = inBytes == hidpp::kShortLen && outBytes == hidpp::kShortLen;
    isLong = inBytes == hidpp::kLongLen && outBytes == hidpp::kLongLen;
    if (!shortOk && !isLong) return nullptr;
    p->inLen = caps.InputReportByteLength;
    p->outLen = caps.OutputReportByteLength;
    p->readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    p->writeEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!p->readEvent || !p->writeEvent) return nullptr;

    // Collections of one interface differ only by "&colNN"; strip it to group them.
    // The instance part also ends in a per-collection counter ("...&0&0001"), so drop that too:
    //   hid#vid_046d&pid_c52b&mi_02&col02#7&1ebf1dd&0&0001#{guid}  ->  hid#vid_046d&pid_c52b&mi_02#7&1ebf1dd&0
    key = Lower(path);
    size_t pos = key.find(L"&col");
    if (pos != std::wstring::npos) {
        size_t instStart = key.find(L'#', pos);
        size_t instEnd = instStart == std::wstring::npos ? instStart : key.find(L'#', instStart + 1);
        if (instEnd != std::wstring::npos) {
            std::wstring inst = key.substr(instStart, instEnd - instStart);
            size_t amp = inst.rfind(L'&');
            if (amp != std::wstring::npos) inst.erase(amp);
            key = key.substr(0, pos) + inst;
        }
    }
    return p;
}

void ProbeChannel(Channel& ch, std::vector<BatteryInfo>& out) {
    hidpp::Transport t;
    t.canShort = ch.shortPipe != nullptr;
    t.canLong = ch.longPipe != nullptr;
    t.write = [&ch](const uint8_t* d, size_t n) { return ch.write(d, n); };
    t.read = [&ch](uint8_t* b, size_t cap, int ms) { return ch.read(b, cap, ms); };

    for (const hidpp::FoundDevice& d : hidpp::discover(t, kIoTimeoutMs)) {
        BatteryInfo b;
        b.name = FromUtf8(d.name);
        if (b.name.empty()) b.name = L"Logitech device";
        b.kind = d.kind;
        b.percent = d.battery.percent;
        b.charging = d.battery.charging;
        b.approximate = d.battery.approximate;
        b.id = L"hidpp:" + ch.key + L":" + std::to_wstring(d.index);
        out.push_back(b);
    }
}

}  // namespace

std::vector<BatteryInfo> QueryHidpp() {
    std::vector<BatteryInfo> out;
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);
    HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;

    // Group the short and long collections of each interface into one channel.
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
        bool isLong = false;
        std::wstring key;
        std::unique_ptr<Pipe> pipe = OpenPipe(path, isLong, key);
        if (!pipe) continue;
        std::unique_ptr<Channel>& ch = chosen[key];
        if (!ch) {
            ch = std::make_unique<Channel>();
            ch->key = key;
        }
        std::unique_ptr<Pipe>& slot = isLong ? ch->longPipe : ch->shortPipe;
        if (!slot) slot = std::move(pipe);
    }
    SetupDiDestroyDeviceInfoList(set);

    for (auto& kv : chosen) ProbeChannel(*kv.second, out);
    return out;
}
