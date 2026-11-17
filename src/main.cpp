// BatteryBar: one notification-area icon per wireless device, showing its
// battery percentage. Polling runs on a worker thread; the UI thread only
// draws icons and handles messages.
#include "device.h"

#include <windows.h>
#include <windowsx.h>
#include <dbt.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <map>
#include <memory>
#include <utility>

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;    // tray callback (NOTIFYICON_VERSION_4 semantics)
constexpr UINT WM_RESULT = WM_APP + 2;  // lParam: heap-allocated std::vector<BatteryInfo>*
constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_DEBOUNCE = 2;
constexpr UINT POLL_MS = 60000;
constexpr UINT DEBOUNCE_MS = 2000;
constexpr ULONGLONG STALE_KEEP_MS = 30 * 60 * 1000;  // how long a silent device keeps its icon
enum MenuId { ID_REFRESH = 100, ID_AUTOSTART, ID_EXIT };

const wchar_t kClassName[] = L"BatteryBarWindow";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"BatteryBar";
const wchar_t kNoDevicesId[] = L"none";

struct Tray {
    UINT uid = 0;
    HICON icon = nullptr;
    bool added = false;
};

struct App {
    HWND hwnd = nullptr;
    UINT taskbarCreated = 0;
    std::vector<BatteryInfo> devices;
    std::map<std::wstring, Tray> trays;  // keyed by BatteryInfo::id
    UINT nextUid = 1;
    HANDLE wake = nullptr;    // auto-reset event: "poll now"
    HANDLE worker = nullptr;
    volatile LONG quit = 0;
} g;

// ---- Polling ---------------------------------------------------------------

std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool NamesOverlap(const std::wstring& a, const std::wstring& b) {
    std::wstring x = LowerCopy(a), y = LowerCopy(b);
    if (x.size() < 3 || y.size() < 3) return false;
    return x.find(y) != std::wstring::npos || y.find(x) != std::wstring::npos;
}

// Runs all providers and merges the results, preferring HID++ over Bluetooth
// over XInput when the same device shows up more than once.
std::vector<BatteryInfo> Collect() {
    std::vector<BatteryInfo> all;
    try {
        all = QueryHidpp();
        const size_t hidCount = all.size();
        bool btController = false;
        for (BatteryInfo& b : QueryBluetooth()) {
            bool dup = false;
            for (size_t i = 0; i < hidCount; ++i) dup = dup || NamesOverlap(all[i].name, b.name);
            if (dup) continue;
            btController = btController || b.kind == DeviceKind::Controller;
            all.push_back(b);
        }
        // XInput slots cannot be matched to Bluetooth devices, so skip them if
        // a Bluetooth controller was already reported.
        if (!btController)
            for (BatteryInfo& x : QueryXInput()) all.push_back(x);
    } catch (...) {
        // A provider failed unexpectedly: show whatever we have.
    }
    std::stable_sort(all.begin(), all.end(), [](const BatteryInfo& a, const BatteryInfo& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
    return all;
}

// A sleeping Logitech device stops answering until it is next used. Keep its
// last reading for a while, marked as stale, so the icon does not flicker.
std::vector<BatteryInfo> Retain(std::vector<BatteryInfo> fresh) {
    static std::map<std::wstring, std::pair<BatteryInfo, ULONGLONG>> seen;
    const ULONGLONG now = GetTickCount64();
    for (const BatteryInfo& b : fresh)
        if (b.id.compare(0, 6, L"hidpp:") == 0) seen[b.id] = {b, now};
    for (auto it = seen.begin(); it != seen.end();) {
        if (now - it->second.second > STALE_KEEP_MS) {
            it = seen.erase(it);
            continue;
        }
        const BatteryInfo& old = it->second.first;
        bool present = std::any_of(fresh.begin(), fresh.end(), [&](const BatteryInfo& b) {
            return b.id == old.id || (b.kind == old.kind && NamesOverlap(b.name, old.name));
        });
        if (!present) {
            BatteryInfo b = old;
            b.stale = true;
            b.charging = false;
            fresh.push_back(b);
        }
        ++it;
    }
    std::stable_sort(fresh.begin(), fresh.end(), [](const BatteryInfo& a, const BatteryInfo& b) {
        return a.kind != b.kind ? a.kind < b.kind : a.name < b.name;
    });
    return fresh;
}

DWORD WINAPI WorkerMain(LPVOID) {
    for (;;) {
        WaitForSingleObject(g.wake, INFINITE);
        if (g.quit) break;
        auto* result = new std::vector<BatteryInfo>(Collect());
        if (g.quit || !PostMessageW(g.hwnd, WM_RESULT, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }
    return 0;
}

void RequestPoll() { SetEvent(g.wake); }

// ---- Icon drawing ----------------------------------------------------------

COLORREF LevelColour(int percent) {
    if (percent < 0) return RGB(170, 170, 170);
    if (percent > 50) return RGB(80, 220, 100);
    if (percent > 20) return RGB(255, 193, 7);
    return RGB(255, 82, 82);
}

uint32_t KindBand(DeviceKind k) {  // 0xRRGGBB
    switch (k) {
        case DeviceKind::Mouse: return 0x4FC3F7;       // blue: M
        case DeviceKind::Keyboard: return 0xB388FF;    // violet: K
        case DeviceKind::Headset: return 0xF06292;     // pink: H
        case DeviceKind::Controller: return 0xFFFFFF;  // white: C
        default: return 0x909090;
    }
}

wchar_t KindLetter(DeviceKind k) {
    switch (k) {
        case DeviceKind::Mouse: return L'M';
        case DeviceKind::Keyboard: return L'K';
        case DeviceKind::Headset: return L'H';
        case DeviceKind::Controller: return L'C';
        default: return L'?';
    }
}

// Rounded-square mask; r is the corner radius in pixels.
bool InsideShape(int x, int y, int s, int r) {
    int dx = x < r ? r - 1 - x : (x >= s - r ? x - (s - r) : -1);
    int dy = y < r ? r - 1 - y : (y >= s - r ? y - (s - r) : -1);
    return dx < 0 || dy < 0 || dx * dx + dy * dy < r * r;
}

// Draws a dark rounded square with the percentage in a level colour, a band
// along the bottom for the device kind, and a white top band while charging.
HICON MakeIcon(const BatteryInfo& b) {
    int s = std::max(16, GetSystemMetrics(SM_CXSMICON));
    int band = std::max(2, s / 8);
    int radius = std::max(2, s / 6);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s;
    bi.bmiHeader.biHeight = -s;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !color || !bits) {
        if (color) DeleteObject(color);
        if (dc) DeleteDC(dc);
        return nullptr;
    }
    HGDIOBJ oldBmp = SelectObject(dc, color);
    auto* px = static_cast<uint32_t*>(bits);

    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            uint32_t c = 0;
            if (InsideShape(x, y, s, radius)) {
                c = 0xFF202020;
                if (y >= s - band) c = 0xFF000000 | KindBand(b.kind);
                else if (b.charging && y < band) c = 0xFFFFFFFF;
            }
            px[y * s + x] = c;
        }
    }

    std::wstring text = b.percent < 0 ? L"?" : std::to_wstring(std::min(b.percent, 100));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, LevelColour(b.percent));
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    for (int h = s - 2 * band; h >= 6; --h) {  // shrink until the text fits
        HFONT f = CreateFontW(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!f) break;
        HGDIOBJ prev = SelectObject(dc, f);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz);
        if (font) DeleteObject(font);
        font = f;
        if (!oldFont) oldFont = prev;
        if (sz.cx <= s - 2) break;
    }
    RECT rc = {0, band, s, s - band};
    DrawTextW(dc, text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    // GDI text leaves the alpha byte undefined: set it from the shape mask.
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            uint32_t& p = px[y * s + x];
            p = InsideShape(x, y, s, radius) ? (p | 0xFF000000u) : 0u;
        }

    std::vector<BYTE> zeros(static_cast<size_t>((s + 15) / 16) * 2 * static_cast<size_t>(s), 0);
    HBITMAP mask = CreateBitmap(s, s, 1, 1, zeros.data());
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    if (oldFont) SelectObject(dc, oldFont);
    SelectObject(dc, oldBmp);
    if (font) DeleteObject(font);
    if (mask) DeleteObject(mask);
    DeleteObject(color);
    DeleteDC(dc);
    return icon;
}

// ---- Tray ------------------------------------------------------------------

std::wstring Tooltip(const BatteryInfo& b) {
    if (b.percent < 0) return b.name;
    std::wstring t = b.name + L": " + (b.approximate ? L"~" : L"") + std::to_wstring(b.percent) + L"%";
    if (b.charging) t += L" (charging)";
    if (b.stale) t += L" (asleep, last reading)";
    return t;
}

NOTIFYICONDATAW BaseNid(const Tray& t) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g.hwnd;
    nid.uID = t.uid;
    return nid;
}

void ShowIcon(Tray& t, const BatteryInfo& b) {
    NOTIFYICONDATAW nid = BaseNid(t);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = t.icon;
    lstrcpynW(nid.szTip, Tooltip(b).c_str(), ARRAYSIZE(nid.szTip));
    if (t.added) {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        return;
    }
    nid.uFlags |= NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        t.added = true;
    }
}

void RemoveIcon(Tray& t) {
    if (t.added) {
        NOTIFYICONDATAW nid = BaseNid(t);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        t.added = false;
    }
    if (t.icon) DestroyIcon(t.icon);
    t.icon = nullptr;
}

// Reconciles tray icons with the device list: add, update, remove.
void ApplyDevices(std::vector<BatteryInfo> list) {
    if (list.empty()) {  // keep one icon so the menu (and Exit) stays reachable
        BatteryInfo none;
        none.name = L"No devices found";
        none.id = kNoDevicesId;
        list.push_back(none);
    }
    g.devices = list;

    for (const BatteryInfo& b : g.devices) {
        auto it = g.trays.find(b.id);
        if (it == g.trays.end()) {
            Tray t;
            t.uid = g.nextUid++;
            it = g.trays.emplace(b.id, t).first;
        }
        Tray& t = it->second;
        HICON fresh = MakeIcon(b);
        if (!fresh) continue;
        HICON old = t.icon;
        t.icon = fresh;
        ShowIcon(t, b);
        if (old) DestroyIcon(old);  // only after the shell has the new one
    }
    for (auto it = g.trays.begin(); it != g.trays.end();) {
        bool live = std::any_of(g.devices.begin(), g.devices.end(), [&](const BatteryInfo& b) { return b.id == it->first; });
        if (live) {
            ++it;
        } else {
            RemoveIcon(it->second);
            it = g.trays.erase(it);
        }
    }
}

// ---- Autostart and menu ----------------------------------------------------

bool AutostartEnabled() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    bool present = RegQueryValueExW(key, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}

void SetAutostart(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, ARRAYSIZE(path) - 1);
        std::wstring value = std::wstring(L"\"") + path + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
}

void ShowMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (const BatteryInfo& b : g.devices) {
        std::wstring line = b.id == kNoDevicesId ? b.name : Tooltip(b);
        std::wstring label = b.percent >= 0 ? std::wstring(1, KindLetter(b.kind)) + L"  " + line : line;
        AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, label.c_str());
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(g.hwnd);  // required so the menu closes when clicking elsewhere
    UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, x, y, 0, g.hwnd, nullptr));
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == ID_REFRESH) RequestPoll();
    else if (cmd == ID_AUTOSTART) SetAutostart(!AutostartEnabled());
    else if (cmd == ID_EXIT) DestroyWindow(g.hwnd);
}

// ---- Window ----------------------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && msg != 0) {  // Explorer restarted: icons are gone
        for (auto& kv : g.trays) kv.second.added = false;
        ApplyDevices(g.devices);
        return 0;
    }
    switch (msg) {
        case WM_TRAY:
            // Version 4 sends WM_CONTEXTMENU after the right-button messages; handle only that one.
            if (LOWORD(lp) == WM_CONTEXTMENU) ShowMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            return 0;
        case WM_RESULT: {
            std::unique_ptr<std::vector<BatteryInfo>> result(reinterpret_cast<std::vector<BatteryInfo>*>(lp));
            ApplyDevices(Retain(std::move(*result)));
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_POLL) {
                RequestPoll();
            } else if (wp == TIMER_DEBOUNCE) {
                KillTimer(hwnd, TIMER_DEBOUNCE);
                RequestPoll();
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED || wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
                SetTimer(hwnd, TIMER_DEBOUNCE, DEBOUNCE_MS, nullptr);  // restarts the timer on every event
            return TRUE;
        case WM_DPICHANGED:
        case WM_DISPLAYCHANGE:
            ApplyDevices(g.devices);  // redraw at the new icon size
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_DEBOUNCE);
            for (auto& kv : g.trays) RemoveIcon(kv.second);
            g.trays.clear();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\BatteryBar.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return 0;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;

    // A hidden top-level window (not message-only) so it receives the
    // broadcast WM_DEVICECHANGE and TaskbarCreated messages.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"BatteryBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    ApplyDevices({});  // placeholder icon until the first poll finishes
    SetTimer(g.hwnd, TIMER_POLL, POLL_MS, nullptr);
    RequestPoll();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    InterlockedExchange(&g.quit, 1);
    SetEvent(g.wake);
    if (g.worker) {
        WaitForSingleObject(g.worker, 3000);  // a poll may be mid-I/O; do not hang on exit
        CloseHandle(g.worker);
    }
    if (g.wake) CloseHandle(g.wake);
    if (mutex) CloseHandle(mutex);
    return 0;
}
