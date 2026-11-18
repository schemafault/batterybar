// BatteryBar: one taskbar button per wireless device, showing a picture of the
// device and its battery percentage. Polling runs on a worker thread; the UI
// thread only draws icons and handles messages.
#include "appearance.h"
#include "device.h"
#include "roster.h"

#include <windows.h>
#include <dbt.h>
#include <objbase.h>
#include <propsys.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>

namespace {

constexpr UINT WM_SHOWMENU = WM_APP + 1;  // a taskbar button was clicked
constexpr UINT WM_RESULT = WM_APP + 2;    // lParam: heap-allocated ProviderReadings*
constexpr UINT_PTR TIMER_POLL = 1;
constexpr UINT_PTR TIMER_DEBOUNCE = 2;
constexpr UINT POLL_MS = 60000;
constexpr UINT DEBOUNCE_MS = 2000;
enum MenuId { ID_REFRESH = 100, ID_AUTOSTART, ID_EXIT };

const wchar_t kClassName[] = L"BatteryBarWindow";
const wchar_t kButtonClass[] = L"BatteryBarButton";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"BatteryBar";
const wchar_t kNoDevicesId[] = L"none";

// PKEY_AppUserModel_ID, spelled out so no extra import library is needed.
const PROPERTYKEY kAppUserModelIdKey = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};

struct Button {
    HWND hwnd = nullptr;
    HICON bigIcon = nullptr, smallIcon = nullptr;
    std::wstring state;  // what the button currently shows, to skip needless updates
};

struct App {
    HWND hwnd = nullptr;
    Roster roster;                           // UI thread only
    std::vector<BatteryInfo> devices;
    std::map<std::wstring, Button> buttons;  // keyed by BatteryInfo::id
    HANDLE wake = nullptr;    // auto-reset event: "poll now"
    HANDLE worker = nullptr;
    volatile LONG quit = 0;
} g;

// ---- Polling ---------------------------------------------------------------

// Asks every provider for its readings. Which of them end up on the taskbar
// is the roster's decision, made on the UI thread.
ProviderReadings Collect() {
    ProviderReadings r;
    try {
        r.hidpp = QueryHidpp();
        r.bluetooth = QueryBluetooth();
        r.xinput = QueryXInput();
    } catch (...) {
        // A provider failed unexpectedly: show whatever we have.
    }
    return r;
}

DWORD WINAPI WorkerMain(LPVOID) {
    for (;;) {
        WaitForSingleObject(g.wake, INFINITE);
        if (g.quit) break;
        auto* result = new ProviderReadings(Collect());
        if (g.quit || !PostMessageW(g.hwnd, WM_RESULT, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }
    return 0;
}

void RequestPoll() { SetEvent(g.wake); }

// ---- Icon drawing ----------------------------------------------------------

// Turns the appearance module's description into an icon. What the icon shows
// is decided there; this only adds the number, which needs GDI text.
HICON MakeIcon(const BatteryInfo& b, int s) {
    const ButtonAppearance look = Describe(b);
    const std::vector<uint32_t> backdrop = RenderBackdrop(b, s);
    if (s <= 0 || backdrop.size() != static_cast<size_t>(s) * static_cast<size_t>(s)) return nullptr;

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
    for (size_t i = 0; i < backdrop.size(); ++i) px[i] = backdrop[i] & 0x00FFFFFFu;

    const TextBand band = TextBandFor(s);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB((look.textColour >> 16) & 0xFF, (look.textColour >> 8) & 0xFF, look.textColour & 0xFF));
    HFONT font = nullptr;
    HGDIOBJ oldFont = nullptr;
    for (int h = band.bottom - band.top; h >= 6; --h) {  // shrink until the text fits
        HFONT f = CreateFontW(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        if (!f) break;
        HGDIOBJ prev = SelectObject(dc, f);
        SIZE sz = {};
        GetTextExtentPoint32W(dc, look.text.c_str(), static_cast<int>(look.text.size()), &sz);
        if (font) DeleteObject(font);
        font = f;
        if (!oldFont) oldFont = prev;
        if (sz.cx <= s - std::max(2, s / 10)) break;
    }
    RECT rc = {0, band.top, s, band.bottom};
    DrawTextW(dc, look.text.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();

    // GDI text leaves the alpha byte undefined: take it from the backdrop.
    for (size_t i = 0; i < backdrop.size(); ++i) {
        uint32_t alpha = backdrop[i] & 0xFF000000u;
        px[i] = alpha ? ((px[i] & 0x00FFFFFFu) | alpha) : 0u;
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

// ---- Taskbar buttons -------------------------------------------------------

// The taskbar groups windows by application id. Give each device its own, so
// every device gets a separate button and keeps its place on the taskbar.
void SetAppId(HWND hwnd, const std::wstring& deviceId) {
    uint32_t h = 2166136261u;  // FNV-1a
    for (wchar_t c : deviceId) h = (h ^ static_cast<uint32_t>(c)) * 16777619u;
    wchar_t appId[64];
    wsprintfW(appId, L"BatteryBar.Device.%08X", h);

    IPropertyStore* store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_IPropertyStore, reinterpret_cast<void**>(&store))) || !store) return;
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_LPWSTR;
    value.pwszVal = appId;  // borrowed: not cleared
    store->SetValue(kAppUserModelIdKey, value);
    store->Release();
}

void DestroyButton(Button& b) {
    if (b.hwnd) DestroyWindow(b.hwnd);
    if (b.bigIcon) DestroyIcon(b.bigIcon);
    if (b.smallIcon) DestroyIcon(b.smallIcon);
    b = Button();
}

// Reconciles taskbar buttons with the device list: add, update, remove.
// Each button is a window that stays minimised; its icon and title carry the reading.
void ApplyDevices(std::vector<BatteryInfo> list, bool redraw = false) {
    if (list.empty()) {  // keep one button so the menu (and Exit) stays reachable
        BatteryInfo none;
        none.name = L"No devices found";
        none.id = kNoDevicesId;
        list.push_back(none);
    }
    g.devices = list;

    for (const BatteryInfo& b : g.devices) {
        Button& btn = g.buttons[b.id];
        std::wstring label = Describe(b).label;
        std::wstring state = label + L"|" + std::to_wstring(static_cast<int>(b.kind));
        if (!btn.hwnd) {
            btn.hwnd = CreateWindowExW(WS_EX_APPWINDOW, kButtonClass, label.c_str(),
                                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                       CW_USEDEFAULT, 300, 120, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!btn.hwnd) continue;
            SetAppId(btn.hwnd, b.id);
        } else if (state == btn.state && !redraw) {
            continue;  // nothing changed: leave the button alone
        }
        btn.state = state;
        SetWindowTextW(btn.hwnd, label.c_str());

        // The taskbar scales the large icon down, so draw it with pixels to spare.
        HICON bigIcon = MakeIcon(b, 2 * GetSystemMetrics(SM_CXICON));
        HICON smallIcon = MakeIcon(b, GetSystemMetrics(SM_CXSMICON));
        if (bigIcon) SendMessageW(btn.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));
        if (smallIcon) SendMessageW(btn.hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
        if (bigIcon && btn.bigIcon) DestroyIcon(btn.bigIcon);  // only after the window has the new one
        if (smallIcon && btn.smallIcon) DestroyIcon(btn.smallIcon);
        if (bigIcon) btn.bigIcon = bigIcon;
        if (smallIcon) btn.smallIcon = smallIcon;
        if (!IsWindowVisible(btn.hwnd)) ShowWindow(btn.hwnd, SW_SHOWMINNOACTIVE);
    }
    for (auto it = g.buttons.begin(); it != g.buttons.end();) {
        bool live = std::any_of(g.devices.begin(), g.devices.end(), [&](const BatteryInfo& b) { return b.id == it->first; });
        if (live) {
            ++it;
        } else {
            DestroyButton(it->second);
            it = g.buttons.erase(it);
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
        std::wstring line = b.id == kNoDevicesId ? b.name : Describe(b).label;
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

// ---- Windows ---------------------------------------------------------------

// A device's taskbar button. The window never opens: clicking the button asks
// to restore it, which shows the menu instead.
LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_QUERYOPEN:
            PostMessageW(g.hwnd, WM_SHOWMENU, 0, 0);
            return FALSE;  // stay minimised
        case WM_CLOSE:  // "Close window" on the taskbar button exits the app
            PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SHOWMENU: {
            MSG extra;  // one click can ask more than once: show a single menu
            while (PeekMessageW(&extra, hwnd, WM_SHOWMENU, WM_SHOWMENU, PM_REMOVE)) {}
            POINT pt = {};
            GetCursorPos(&pt);
            ShowMenu(pt.x, pt.y);
            return 0;
        }
        case WM_RESULT: {
            std::unique_ptr<ProviderReadings> result(reinterpret_cast<ProviderReadings*>(lp));
            ApplyDevices(g.roster.update(*result, GetTickCount64()));
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
            ApplyDevices(g.devices, true);  // redraw at the new icon size
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, TIMER_POLL);
            KillTimer(hwnd, TIMER_DEBOUNCE);
            for (auto& kv : g.buttons) DestroyButton(kv.second);
            g.buttons.clear();
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
    const bool comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return 1;
    wc.lpfnWndProc = ButtonProc;
    wc.lpszClassName = kButtonClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return 1;

    // A hidden top-level window (not message-only) so it receives the
    // broadcast WM_DEVICECHANGE messages. It also owns the menu.
    g.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"BatteryBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) return 1;

    g.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.worker = CreateThread(nullptr, 0, WorkerMain, nullptr, 0, nullptr);
    ApplyDevices({});  // placeholder button until the first poll finishes
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
    if (comReady) CoUninitialize();
    return 0;
}
