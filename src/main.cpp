// BatteryBar: one taskbar button per wireless device, showing a picture of the
// device above its battery percentage. Polling runs on a worker thread; the UI
// thread only draws icons and handles messages.
#include "appearance.h"
#include "device.h"
#include "icon.h"
#include "roster.h"

#include <windows.h>
#include <dbt.h>
#include <dwmapi.h>
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
enum MenuId { ID_REFRESH = 100, ID_AUTOSTART, ID_EXIT, ID_STYLE_FIRST = 200 };  // one id per IconStyle from ID_STYLE_FIRST

const wchar_t kClassName[] = L"BatteryBarWindow";
const wchar_t kButtonClass[] = L"BatteryBarButton";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kRunValue[] = L"BatteryBar";
const wchar_t kSettingsKey[] = L"Software\\BatteryBar";
const wchar_t kStyleValue[] = L"IconStyle";
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
    IconStyle style = IconStyle::PictureAndNumber;
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

// ---- Taskbar theme -------------------------------------------------------

// The icons have no background, so they are drawn light or dark to suit the taskbar.
bool TaskbarIsLight() {
    DWORD value = 0, size = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS &&
           value != 0;
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
    const bool light = TaskbarIsLight();

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
            // The window never opens, so supply the hover preview ourselves
            // (see WM_DWMSENDICONICTHUMBNAIL) and keep "peek" from showing an empty frame.
            const BOOL on = TRUE;
            DwmSetWindowAttribute(btn.hwnd, DWMWA_FORCE_ICONIC_REPRESENTATION, &on, sizeof(on));
            DwmSetWindowAttribute(btn.hwnd, DWMWA_HAS_ICONIC_BITMAP, &on, sizeof(on));
            DwmSetWindowAttribute(btn.hwnd, DWMWA_DISALLOW_PEEK, &on, sizeof(on));
        } else if (state == btn.state && !redraw) {
            continue;  // nothing changed: leave the button alone
        }
        btn.state = state;
        DwmInvalidateIconicBitmaps(btn.hwnd);  // the hover preview is out of date too
        SetWindowTextW(btn.hwnd, label.c_str());

        // The taskbar scales the large icon down, so draw it with pixels to spare.
        HICON bigIcon = MakeIcon(b, 2 * GetSystemMetrics(SM_CXICON), light, g.style);
        HICON smallIcon = MakeIcon(b, GetSystemMetrics(SM_CXSMICON), light, g.style);
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

// ---- Settings, autostart and menu -------------------------------------------

IconStyle LoadStyle() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kStyleValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS ||
        value >= static_cast<DWORD>(kIconStyleCount))
        return IconStyle::PictureAndNumber;
    return static_cast<IconStyle>(value);
}

void SaveStyle(IconStyle style) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD value = static_cast<DWORD>(style);
    RegSetValueExW(key, kStyleValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

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
    if (HMENU styles = CreatePopupMenu()) {  // destroyed along with the menu it is attached to
        for (int i = 0; i < kIconStyleCount; ++i)
            AppendMenuW(styles, MF_STRING, static_cast<UINT_PTR>(ID_STYLE_FIRST + i), StyleName(static_cast<IconStyle>(i)));
        CheckMenuRadioItem(styles, ID_STYLE_FIRST, ID_STYLE_FIRST + kIconStyleCount - 1,
                           static_cast<UINT>(ID_STYLE_FIRST + static_cast<int>(g.style)), MF_BYCOMMAND);
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(styles), L"Icon style");
    }
    AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(g.hwnd);  // required so the menu closes when clicking elsewhere
    UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD, x, y, 0, g.hwnd, nullptr));
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (cmd == ID_REFRESH) RequestPoll();
    else if (cmd == ID_AUTOSTART) SetAutostart(!AutostartEnabled());
    else if (cmd == ID_EXIT) DestroyWindow(g.hwnd);
    else if (cmd >= ID_STYLE_FIRST && cmd < static_cast<UINT>(ID_STYLE_FIRST + kIconStyleCount)) {
        g.style = static_cast<IconStyle>(cmd - ID_STYLE_FIRST);
        SaveStyle(g.style);
        ApplyDevices(g.devices, true);
    }
}

// ---- Windows ---------------------------------------------------------------

// A device's taskbar button. The window never opens: clicking the button asks
// to restore it, which shows the menu instead.
LRESULT CALLBACK ButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DWMSENDICONICTHUMBNAIL: {  // the pointer is resting on the button
            for (const BatteryInfo& b : g.devices) {
                auto it = g.buttons.find(b.id);
                if (it == g.buttons.end() || it->second.hwnd != hwnd) continue;
                if (HBITMAP preview = MakeHoverPreview(b, HIWORD(lp), LOWORD(lp), TaskbarIsLight())) {
                    DwmSetIconicThumbnail(hwnd, preview, 0);
                    DeleteObject(preview);
                }
                break;
            }
            return 0;
        }
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
        case WM_SETTINGCHANGE:  // the taskbar switched between light and dark
            if (lp && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) ApplyDevices(g.devices, true);
            return DefWindowProcW(hwnd, msg, wp, lp);
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

    g.style = LoadStyle();
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
