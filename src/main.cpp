// Wireless Battery - tray app showing the battery level of wireless mice, headsets and Bluetooth devices.
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
#include <initguid.h>
#include <hidclass.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "battery_trend.h"
#include "devices.h"
#include "drivers/registry.h"
#include "json_report.h"
#include "poll_scheduler.h"
#include "widget.h"

namespace {

constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT WM_RESULTS = WM_APP + 2;
constexpr UINT WM_WIDGET_CLICK = WM_APP + 3;
constexpr UINT_PTR TIMER_DEBOUNCE = 1;
constexpr UINT_PTR TIMER_WIDGET = 2;
constexpr UINT PLACEHOLDER_ID = 1;
constexpr UINT NOTICE_ID = 2;  // low-battery icon used while the taskbar widget is shown

// Choices offered in the menu.
constexpr int kLowChoices[] = { 5, 10, 15, 20, 25, 30, 40 };     // percent
constexpr int kIntervalChoices[] = { 30, 60, 120, 300, 600 };   // seconds
constexpr int kDefaultLow = 15;
constexpr int kDefaultIntervalSec = 60;
constexpr int kMaxIgnored = 32;

enum MenuId : UINT {
    ID_REFRESH = 100, ID_AUTOSTART, ID_HIDE_LAST_KNOWN, ID_WIDGET_BACKGROUND, ID_EXIT, ID_MODE_WIDGET, ID_MODE_TRAY, ID_SUPPORT,
    ID_NOTIFY_LOW, ID_ADAPTIVE,
    ID_LOW_FIRST = 200,       // + index into kLowChoices
    ID_INTERVAL_FIRST = 300,  // + index into kIntervalChoices
    ID_UNIGNORE_FIRST = 400,  // + index into the ignored list
    ID_DEVICE_FIRST = 1000
};
// Per-device commands: ID_DEVICE_FIRST + device index * DEV_STRIDE + command.
enum DeviceCmd : UINT {
    DEV_TOGGLE, DEV_LEFT, DEV_RIGHT, DEV_IGNORE, DEV_LOW_DEFAULT, DEV_LOW_NEVER,
    DEV_LOW_FIRST,  // + index into kLowChoices
    DEV_STRIDE = 16
};

const wchar_t* kAppName = L"Wireless Battery";
const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunValue = L"WirelessBattery";

const wchar_t* kSettingsKey = L"Software\\WirelessBattery";

HINSTANCE g_inst;
HWND g_hwnd;
HANDLE g_refreshEvt, g_stopEvt, g_worker;
UINT g_taskbarCreatedMsg;
bool g_lightTaskbar;
bool g_wantWidget = true;      // user's choice: taskbar widget (default) or tray icons
bool g_useWidget = false;      // what is actually shown right now
bool g_hideLastKnown = false;  // hide (last known) / asleep devices from taskbar and tray
bool g_widgetBackground = false;  // draw a plate behind the taskbar text (transparent taskbar themes)
bool g_notifyLow = true;          // show a notification when a device drops to its warning level
int g_lowDefault = kDefaultLow;   // warning level for devices without their own

// Read by the worker thread.
std::atomic<DWORD> g_pollMs{ kDefaultIntervalSec * 1000 };
std::atomic<bool> g_adaptive{ true };
std::atomic<bool> g_parallel{ true };

struct TrayEntry {
    UINT uid = 0;
    DeviceStatus st;
    int lastPct = -1;        // last known percentage, kept while the device sleeps
    ULONGLONG lastSeen = 0;
    bool added = false;
    bool lowWarned = false;
    bool warnNow = false;    // dropped to its warning level in this poll: notify once
    BatteryTrend trend;      // discharge history for the "~N h left" estimate
    bool trendLoaded = false;
    std::wstring iconSig;    // what the current icon shows, to avoid needless redraws
    HICON icon = nullptr;
};
std::map<std::wstring, TrayEntry> g_entries;
bool g_placeholder = false;
HICON g_placeholderIcon = nullptr;
bool g_notice = false;
HICON g_noticeIcon = nullptr;
std::wstring g_noticeSig;

// ---- helpers ----------------------------------------------------------------------------------

UINT StableId(const std::wstring& key) {
    UINT h = 2166136261u;
    for (wchar_t c : key) { h ^= c; h *= 16777619u; }
    return (h & 0x7FFFFFFF) | 0x100;
}

bool ReadLightTaskbar() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS)
        return v != 0;
    return false;
}

void ApplyMenuTheme(bool light) {
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setMode = ux ? (SetPreferredAppModeFn)GetProcAddress(ux, MAKEINTRESOURCEA(135)) : nullptr;
    static auto flush = ux ? (FlushMenuThemesFn)GetProcAddress(ux, MAKEINTRESOURCEA(136)) : nullptr;
    if (setMode) setMode(light ? 3 : 2);
    if (flush) flush();
}

std::wstring ExePath() {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return p;
}

// Devices the user chose to hide (by key), stored as a REG_MULTI_SZ.
std::set<std::wstring> g_hidden;

void LoadHidden() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_hidden.insert(p);
}

void SaveHidden() {
    std::wstring multi;
    for (const auto& k : g_hidden) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"HiddenDevices", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsHidden(const std::wstring& key) { return g_hidden.count(key) != 0; }

// The user's left-to-right order (device keys), stored as a REG_MULTI_SZ. Devices not in it follow
// in the default order (by type, then name).
std::vector<std::wstring> g_order;

void LoadOrder() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) g_order.push_back(p);
}

void SaveOrder() {
    if (g_order.size() > 64) g_order.resize(64);  // forget long-gone devices
    std::wstring multi;
    for (const auto& k : g_order) { multi += k; multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceOrder", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

bool IsEntryHidden(const TrayEntry& e) {
    return IsHidden(e.st.key) || (g_hideLastKnown && (e.st.asleep || (e.st.percent < 0 && e.lastPct >= 0)));
}

bool LoadBoolSetting(const wchar_t* name, bool defaultVal) {
    DWORD v = defaultVal ? 1 : 0, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}

void SaveBoolSetting(const wchar_t* name, bool on) {
    DWORD v = on ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

bool LoadWidgetMode() { return LoadBoolSetting(L"TaskbarWidget", true); }
void SaveWidgetMode(bool on) { SaveBoolSetting(L"TaskbarWidget", on); }
bool LoadHideLastKnown() { return LoadBoolSetting(L"HideLastKnownDevices", false); }
void SaveHideLastKnown(bool on) { SaveBoolSetting(L"HideLastKnownDevices", on); }
bool LoadWidgetBackground() { return LoadBoolSetting(L"WidgetBackground", false); }
void SaveWidgetBackground(bool on) { SaveBoolSetting(L"WidgetBackground", on); }

DWORD LoadDwordSetting(const wchar_t* name, DWORD defaultVal) {
    DWORD v = defaultVal, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS) return defaultVal;
    return v;
}
void SaveDwordSetting(const wchar_t* name, DWORD v) { RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v)); }

template <size_t N>
bool OneOf(int v, const int (&choices)[N]) { return std::find(std::begin(choices), std::end(choices), v) != std::end(choices); }

// Per-device warning levels (device key -> percent, 0 = never warn), stored as a REG_MULTI_SZ of
// "key<TAB>percent" strings. Devices not listed use g_lowDefault.
std::map<std::wstring, int> g_deviceLow;

void LoadDeviceLow() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceLowBattery", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || !size)
        return;
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceLowBattery", RRF_RT_REG_MULTI_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        std::wstring s = p;
        size_t tab = s.rfind(L'\t');
        if (tab == std::wstring::npos) continue;
        int v = _wtoi(s.c_str() + tab + 1);
        if (v == 0 || OneOf(v, kLowChoices)) g_deviceLow[s.substr(0, tab)] = v;
    }
}

void SaveDeviceLow() {
    std::wstring multi;
    for (const auto& [k, v] : g_deviceLow) { multi += k + L"\t" + std::to_wstring(v); multi.push_back(L'\0'); }
    multi.push_back(L'\0');
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"DeviceLowBattery", REG_MULTI_SZ, multi.data(), (DWORD)(multi.size() * sizeof(wchar_t)));
}

// Warning level for a device: percent, or 0 = never warn.
int LowFor(const std::wstring& key) {
    auto it = g_deviceLow.find(key);
    return it != g_deviceLow.end() ? it->second : g_lowDefault;
}

void LoadSettings() {
    g_notifyLow = LoadBoolSetting(L"LowBatteryNotifications", true);
    int low = (int)LoadDwordSetting(L"LowBattery", kDefaultLow);
    g_lowDefault = OneOf(low, kLowChoices) ? low : kDefaultLow;
    int sec = (int)LoadDwordSetting(L"PollSeconds", kDefaultIntervalSec);
    g_pollMs = (DWORD)(OneOf(sec, kIntervalChoices) ? sec : kDefaultIntervalSec) * 1000;
    g_adaptive = LoadBoolSetting(L"AdaptivePolling", true);
    g_parallel = LoadBoolSetting(L"ParallelPolling", true);  // registry only, for troubleshooting
    LoadDeviceLow();
}

// Discharge history per device (battery_trend.h), in HKCU\Software\WirelessBattery\Trends, one REG_SZ
// per device key. Written only when the level drops to a new low or the device is charged.
const wchar_t* kTrendsKey = L"Software\\WirelessBattery\\Trends";

int64_t WallClock() { return (int64_t)time(nullptr); }

BatteryTrend LoadTrend(const std::wstring& key) {
    wchar_t buf[128];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kTrendsKey, key.c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    std::string s;
    for (const wchar_t* p = buf; *p; p++) s.push_back((char)*p);  // ASCII digits and commas
    return BatteryTrend::Parse(s, WallClock());
}

void SaveTrend(const std::wstring& key, const BatteryTrend& t) {
    std::string s = t.Serialize();
    if (s.empty()) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kTrendsKey, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, key.c_str());
            RegCloseKey(k);
        }
        return;
    }
    std::wstring w(s.begin(), s.end());
    RegSetKeyValueW(HKEY_CURRENT_USER, kTrendsKey, key.c_str(), REG_SZ, w.c_str(), (DWORD)((w.size() + 1) * sizeof(wchar_t)));
}

bool AutostartEnabled() {
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    return _wcsicmp(buf, (L"\"" + ExePath() + L"\"").c_str()) == 0;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath() + L"\"";
        RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kRunValue);
    }
    RegCloseKey(k);
}

// ---- icon rendering ---------------------------------------------------------------------------
// Draws the percentage as large digits with a small level bar underneath. GDI text is rendered
// white-on-black into a scratch bitmap and its brightness is used as the alpha channel, which
// gives smooth edges on the transparent icon.

struct Rgb { BYTE r, g, b; };

HICON RenderIcon(const std::wstring& text, int barPct, Rgb textColor, Rgb barColor, bool dim) {
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    UINT dpi = taskbar ? GetDpiForWindow(taskbar) : 0;
    const int S = GetSystemMetricsForDpi(SM_CXSMICON, dpi ? dpi : GetDpiForSystem());
    const int barH = std::max(2, S / 8);
    const int textH = S - barH - 1;

    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    // scratch bitmap for the glyphs
    const int W = S * 3, H = S * 2;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    void* sbits = nullptr;
    HBITMAP scratch = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &sbits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(dc, scratch);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    auto* sp = static_cast<DWORD*>(sbits);

    int bx0 = 0, by0 = 0, bx1 = -1, by1 = -1;
    for (int fh = textH * 2; fh >= 6; fh--) {
        HFONT f = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldF = SelectObject(dc, f);
        SetTextCharacterExtra(dc, text.size() >= 3 ? -1 : 0);
        memset(sbits, 0, (size_t)W * H * 4);
        TextOutW(dc, 2, 0, text.c_str(), (int)text.size());
        GdiFlush();
        SelectObject(dc, oldF);
        DeleteObject(f);
        bx0 = W; by0 = H; bx1 = -1; by1 = -1;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (sp[y * W + x] & 0xFF) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); by0 = std::min(by0, y); by1 = std::max(by1, y); }
        if (bx1 < 0) break;
        if (bx1 - bx0 + 1 <= S && by1 - by0 + 1 <= textH) break;
    }

    // final icon bitmap
    bi.bmiHeader.biWidth = S;
    bi.bmiHeader.biHeight = -S;
    void* ibits = nullptr;
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &ibits, nullptr, 0);
    auto* ip = static_cast<DWORD*>(ibits);
    memset(ip, 0, (size_t)S * S * 4);
    const int alphaScale = dim ? 140 : 255;
    auto put = [&](int x, int y, Rgb c, int a) {
        if (x < 0 || y < 0 || x >= S || y >= S || a <= 0) return;
        a = a * alphaScale / 255;
        ip[y * S + x] = ((DWORD)a << 24) | ((DWORD)c.r << 16) | ((DWORD)c.g << 8) | c.b;  // straight alpha
    };
    if (bx1 >= 0) {
        int gw = bx1 - bx0 + 1, gh = by1 - by0 + 1;
        int ox = (S - gw) / 2, oy = (textH - gh) / 2;
        for (int y = 0; y < gh; y++)
            for (int x = 0; x < gw; x++)
                put(ox + x, oy + y, textColor, sp[(by0 + y) * W + (bx0 + x)] & 0xFF);
    }
    // level bar: faint track + filled part
    int fill = barPct < 0 ? 0 : std::max(barPct > 0 ? 1 : 0, (barPct * S + 50) / 100);
    for (int y = S - barH; y < S; y++)
        for (int x = 0; x < S; x++) {
            if (x < fill) put(x, y, barColor, 255);
            else put(x, y, g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, 70);
        }

    SelectObject(dc, oldBmp);
    DeleteObject(scratch);
    DeleteDC(dc);

    HBITMAP mask = CreateBitmap(S, S, 1, 1, nullptr);
    {
        HDC mdc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(mdc, mask);
        PatBlt(mdc, 0, 0, S, S, BLACKNESS);
        SelectObject(mdc, o);
        DeleteDC(mdc);
    }
    ICONINFO ii{ TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

Rgb Foreground() { return g_lightTaskbar ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }; }

bool IsLowLevel(int pct, bool charging, int low) { return low > 0 && !charging && pct >= 0 && pct <= low; }

// `low` is the device's warning level (0 = never warn).
HICON IconFor(int pct, bool charging, bool dim, int low) {
    Rgb fg = Foreground();
    Rgb bar = charging                   ? Rgb{ 0, 170, 255 }
            : IsLowLevel(pct, false, low) ? Rgb{ 232, 32, 48 }
            : pct <= 30                  ? Rgb{ 255, 150, 0 }
                                         : Rgb{ 30, 190, 90 };
    Rgb text = IsLowLevel(pct, charging, low) ? Rgb{ 240, 50, 60 } : fg;
    std::wstring t = pct < 0 ? L"?" : std::to_wstring(pct);
    return RenderIcon(t, pct, text, bar, dim);
}

// ---- tray management --------------------------------------------------------------------------

void CopyTip(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy(dst, s.c_str(), cap - 1);
    dst[cap - 1] = 0;
}

std::wstring StatusText(const TrayEntry& e) {
    const auto& s = e.st;
    if (!s.connected) return L"off / not connected";
    std::wstring t;
    if (!s.levelText.empty() && s.percent >= 0) t = s.levelText;
    else if (s.percent >= 0) t = std::to_wstring(s.percent) + L"%";
    else if (e.lastPct >= 0) t = std::to_wstring(e.lastPct) + L"% (last known)";
    else t = L"unknown";
    if (!s.detail.empty() && s.percent >= 0) t += L" (" + s.detail + L")";
    if (s.charging) t += L", charging";
    if (s.asleep) t += L", asleep";
    if (!s.charging && (s.percent >= 0 || e.lastPct >= 0)) {
        std::wstring left = FormatRemaining(e.trend.RemainingSec(WallClock()));
        if (!left.empty()) t += L", " + left + L" left";
    }
    return t;
}

void TrayCall(DWORD msg, UINT uid, HICON icon, const std::wstring& tip, const wchar_t* infoTitle = nullptr,
              const std::wstring& info = L"") {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon;
    CopyTip(nid.szTip, ARRAYSIZE(nid.szTip), tip);
    if (infoTitle) {
        nid.uFlags |= NIF_INFO;
        CopyTip(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), infoTitle);
        CopyTip(nid.szInfo, ARRAYSIZE(nid.szInfo), info);
        nid.dwInfoFlags = NIIF_WARNING;
    }
    Shell_NotifyIconW(msg, &nid);
    if (msg == NIM_ADD) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
}

void TrayDelete(UINT uid) {
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = g_hwnd;
    nid.uID = uid;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void UpdatePlaceholder() {
    if (g_useWidget) {
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
        return;
    }
    bool any = false;
    for (auto& [k, e] : g_entries) any |= e.added;
    if (!any && !g_placeholder) {
        if (!g_placeholderIcon) g_placeholderIcon = RenderIcon(L"-", -1, Foreground(), Foreground(), true);
        TrayCall(NIM_ADD, PLACEHOLDER_ID, g_placeholderIcon, L"Wireless Battery\nNo devices with battery info found");
        g_placeholder = true;
    } else if (any && g_placeholder) {
        TrayDelete(PLACEHOLDER_ID);
        g_placeholder = false;
    }
}

void ApplyEntry(TrayEntry& e) {
    const auto& s = e.st;
    bool visible = s.connected && !g_useWidget && !IsEntryHidden(e);
    if (!visible) {
        if (e.added) { TrayDelete(e.uid); e.added = false; }
        return;
    }
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    bool dim = s.percent < 0;
    int low = LowFor(s.key);
    std::wstring sig = std::to_wstring(shown) + (s.charging ? L"c" : L"") + (dim ? L"d" : L"") + (g_lightTaskbar ? L"l" : L"k") +
                       L"/" + std::to_wstring(low);
    if (sig != e.iconSig || !e.icon) {
        if (e.icon) DestroyIcon(e.icon);
        e.icon = IconFor(shown, s.charging, dim, low);
        e.iconSig = sig;
    }
    std::wstring tip = s.name + L"\n" + StatusText(e);

    const wchar_t* infoTitle = nullptr;
    std::wstring info;
    if (e.warnNow) {  // see TakeLowWarning
        e.warnNow = false;
        infoTitle = L"Low battery";
        info = s.name + L" is at " + std::to_wstring(s.percent) + L"%";
    }
    TrayCall(e.added ? NIM_MODIFY : NIM_ADD, e.uid, e.icon, tip, infoTitle, info);
    e.added = true;
}

int KindOrder(DeviceKind k) {  // default order: headset first, then mouse, ...
    switch (k) {
        case DeviceKind::Headset:  return 0;
        case DeviceKind::Mouse:    return 1;
        case DeviceKind::Keyboard: return 2;
        case DeviceKind::Gamepad:  return 3;
        case DeviceKind::Phone:    return 4;
        default:                   return 5;
    }
}

bool IsShown(const TrayEntry& e) { return e.st.connected && !IsEntryHidden(e); }

// Entries in left-to-right order: the user's order first, then the rest by type and name. Works the
// same for every kind of device (receivers, Bluetooth, controllers, HeadsetControl) as it only uses
// the device key.
std::vector<TrayEntry*> OrderedEntries(bool shownOnly) {
    std::vector<TrayEntry*> list;
    for (auto& [k, e] : g_entries)
        if (!shownOnly || IsShown(e)) list.push_back(&e);
    auto rank = [](const TrayEntry* e) {
        auto it = std::find(g_order.begin(), g_order.end(), e->st.key);
        return it == g_order.end() ? INT_MAX : (int)(it - g_order.begin());
    };
    std::stable_sort(list.begin(), list.end(), [&](const TrayEntry* a, const TrayEntry* b) {
        int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        int ka = KindOrder(a->st.kind), kb = KindOrder(b->st.kind);
        return ka != kb ? ka < kb : a->st.name < b->st.name;
    });
    return list;
}

void UpdateWidget() {
    if (!g_useWidget) return;
    std::vector<TrayEntry*> list = OrderedEntries(true);
    std::vector<WidgetItem> items;
    std::wstring tip;
    for (auto* e : list) {
        WidgetItem it;
        it.kind = e->st.kind;
        it.percent = e->st.percent >= 0 ? e->st.percent : e->lastPct;
        if (e->st.percent >= 0) it.text = e->st.levelText;
        it.charging = e->st.charging;
        it.dim = e->st.percent < 0;
        it.low = IsLowLevel(it.percent, it.charging, LowFor(e->st.key));
        it.bluetooth = e->st.via == L"Bluetooth";
        items.push_back(it);
        if (!tip.empty()) tip += L"\n";
        tip += e->st.name + L": " + StatusText(*e);
    }
    if (tip.empty()) tip = L"Wireless Battery: no devices with battery info found";
    widget::SetItems(items, tip);
}

// True once per discharge, when a device first reports its warning level or lower. Runs in both
// display modes so switching modes neither repeats nor loses a warning.
bool TakeLowWarning(TrayEntry& e) {
    const auto& s = e.st;
    int low = LowFor(s.key);
    int shown = s.percent >= 0 ? s.percent : e.lastPct;
    if (s.charging || low == 0 || shown > low + 10) e.lowWarned = false;
    if (!IsLowLevel(s.percent, s.charging, low) || e.lowWarned) return false;
    e.lowWarned = true;
    return true;
}

// The taskbar widget is not a tray icon, so it cannot show notifications itself. While it is shown
// and any device is at its warning level, one extra tray icon carries the lowest level (in red, the
// same as the tray mode icons) and shows the "Low battery" notification. Windows puts new icons in
// the overflow area, so it only shows up when there is something to say.
void UpdateNotice() {
    std::vector<TrayEntry*> low, fresh;
    if (g_useWidget && g_notifyLow)
        for (auto& [k, e] : g_entries) {
            int pct = e.st.percent >= 0 ? e.st.percent : e.lastPct;
            if (!IsShown(e) || !IsLowLevel(pct, e.st.charging, LowFor(k))) continue;
            low.push_back(&e);
            if (e.warnNow) fresh.push_back(&e);
        }
    for (auto& [k, e] : g_entries) e.warnNow = false;  // consumed (or not wanted) in widget mode
    if (low.empty()) {
        if (g_notice) { TrayDelete(NOTICE_ID); g_notice = false; }
        return;
    }
    auto lowest = std::min_element(low.begin(), low.end(), [](const TrayEntry* a, const TrayEntry* b) {
        int pa = a->st.percent >= 0 ? a->st.percent : a->lastPct, pb = b->st.percent >= 0 ? b->st.percent : b->lastPct;
        return pa < pb;
    });
    int pct = (*lowest)->st.percent >= 0 ? (*lowest)->st.percent : (*lowest)->lastPct;
    std::wstring tip = L"Low battery";
    for (auto* e : low) tip += L"\n" + e->st.name + L": " + StatusText(*e);
    std::wstring sig = std::to_wstring(pct) + (g_lightTaskbar ? L"l" : L"k");
    if (sig != g_noticeSig || !g_noticeIcon) {
        if (g_noticeIcon) DestroyIcon(g_noticeIcon);
        g_noticeIcon = IconFor(pct, false, false, 100);
        g_noticeSig = sig;
    }
    std::wstring info;
    for (auto* e : fresh) {
        if (!info.empty()) info += L"\n";
        info += e->st.name + L" is at " + std::to_wstring(e->st.percent) + L"%";
    }
    TrayCall(g_notice ? NIM_MODIFY : NIM_ADD, NOTICE_ID, g_noticeIcon, tip, info.empty() ? nullptr : L"Low battery", info);
    g_notice = true;
}

void OnResults(std::vector<DeviceStatus>& results) {
    std::map<std::wstring, bool> seen;
    for (auto& s : results) {
        auto& e = g_entries[s.key];
        if (!e.uid) e.uid = StableId(s.key);
        e.st = s;
        if (s.percent >= 0) e.lastPct = s.percent;
        e.lastSeen = GetTickCount64();
        seen[s.key] = true;
        if (!e.trendLoaded) {
            e.trend = LoadTrend(s.key);
            e.trendLoaded = true;
        }
        // Coarse "Low / Medium / Full" levels say too little about time to estimate anything.
        if (s.connected && !s.asleep && s.percent >= 0 && s.levelText.empty() && e.trend.Update(s.percent, s.charging, WallClock()))
            SaveTrend(s.key, e.trend);
        e.warnNow = TakeLowWarning(e) && g_notifyLow && !IsHidden(s.key);
        ApplyEntry(e);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        auto& e = it->second;
        // Some receivers drop sleeping devices from the link; keep showing the last value for a while.
        bool inGrace = !seen.count(it->first) && e.st.graceSec > 0 && e.st.connected &&
                       GetTickCount64() - e.lastSeen < (ULONGLONG)e.st.graceSec * 1000;
        if (inGrace) {
            e.st.percent = -1;
            e.st.asleep = true;
            ApplyEntry(e);
        }
        if (!seen.count(it->first) && !inGrace) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    UpdatePlaceholder();
    UpdateWidget();
    UpdateNotice();
}

// Makes the actual display match the chosen mode; falls back to tray icons if the taskbar
// widget cannot be created (e.g. Explorer not ready yet). Called once a second.
void Reconcile() {
    bool alive = g_wantWidget && widget::Alive();
    if (g_wantWidget && !alive) alive = widget::Create(g_inst, g_hwnd, WM_WIDGET_CLICK, g_lightTaskbar);
    if (!g_wantWidget) widget::Destroy();
    bool changed = alive != g_useWidget;
    g_useWidget = alive;
    if (changed) {
        for (auto& [k, e] : g_entries) ApplyEntry(e);
        UpdatePlaceholder();
        UpdateWidget();
        UpdateNotice();
    } else if (g_useWidget) {
        widget::Reposition();
    }
}

void SetMode(bool useWidget) {
    g_wantWidget = useWidget;
    SaveWidgetMode(useWidget);
    Reconcile();
}

void SetWidgetBackground(bool on) {
    g_widgetBackground = on;
    SaveWidgetBackground(on);
    widget::SetBackground(on);
}

void RefreshDisplay() {
    for (auto& [k, e] : g_entries) ApplyEntry(e);
    UpdatePlaceholder();
    UpdateWidget();
    UpdateNotice();
}

void SetHideLastKnown(bool on) {
    g_hideLastKnown = on;
    SaveHideLastKnown(on);
    RefreshDisplay();
}

void ReAddAll() {
    g_placeholder = false;
    g_notice = false;
    for (auto& [k, e] : g_entries) { e.added = false; ApplyEntry(e); }
    UpdatePlaceholder();
    UpdateNotice();
}

void RedrawAll() {
    if (g_placeholderIcon) {
        DestroyIcon(g_placeholderIcon);
        g_placeholderIcon = nullptr;
        if (g_placeholder) { TrayDelete(PLACEHOLDER_ID); g_placeholder = false; }
    }
    g_noticeSig.clear();
    for (auto& [k, e] : g_entries) { e.iconSig.clear(); ApplyEntry(e); }
    UpdatePlaceholder();
    UpdateNotice();
}

void ToggleHidden(const std::wstring& key) {
    if (!g_hidden.erase(key)) g_hidden.insert(key);
    SaveHidden();
    RefreshDisplay();
}

void SetNotifyLow(bool on) {
    g_notifyLow = on;
    SaveBoolSetting(L"LowBatteryNotifications", on);
    UpdateNotice();
}

void SetLowDefault(int pct) {
    g_lowDefault = pct;
    SaveDwordSetting(L"LowBattery", (DWORD)pct);
    RefreshDisplay();
}

// pct: a level from kLowChoices, 0 = never warn, -1 = use the default.
void SetDeviceLow(const std::wstring& key, int pct) {
    if (pct < 0) g_deviceLow.erase(key);
    else g_deviceLow[key] = pct;
    SaveDeviceLow();
    RefreshDisplay();
}

void SetInterval(int sec) {
    g_pollMs = (DWORD)sec * 1000;
    SaveDwordSetting(L"PollSeconds", (DWORD)sec);
    SetEvent(g_refreshEvt);  // start the new rhythm now
}

void SetAdaptive(bool on) {
    g_adaptive = on;
    SaveBoolSetting(L"AdaptivePolling", on);
}

std::wstring UsbId(unsigned short vid, unsigned short pid) {
    wchar_t b[16];
    swprintf(b, 16, L"%04X:%04X", vid, pid);
    return b;
}

// Never open this vendor/product id again (HKCU\...\IgnoreDevices), and drop what it showed.
void IgnoreDevice(unsigned short vid, unsigned short pid) {
    auto ids = IgnoredDeviceIds();
    if (std::find(ids.begin(), ids.end(), std::make_pair(vid, pid)) == ids.end() && (int)ids.size() < kMaxIgnored) {
        ids.push_back({ vid, pid });
        SetIgnoredDeviceIds(ids);
    }
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        if (it->second.st.vid == vid && it->second.st.pid == pid) {
            if (it->second.added) TrayDelete(it->second.uid);
            if (it->second.icon) DestroyIcon(it->second.icon);
            it = g_entries.erase(it);
        } else {
            ++it;
        }
    }
    RefreshDisplay();
    SetEvent(g_refreshEvt);
}

void UnignoreDevice(size_t index) {
    auto ids = IgnoredDeviceIds();
    if (index >= ids.size()) return;
    ids.erase(ids.begin() + index);
    SetIgnoredDeviceIds(ids);
    SetEvent(g_refreshEvt);
}

// Swaps a shown device with its neighbour (dir -1 = left, +1 = right) and remembers the new order.
void MoveEntry(const std::wstring& key, int dir) {
    std::vector<TrayEntry*> list = OrderedEntries(true);
    auto pos = std::find_if(list.begin(), list.end(), [&](const TrayEntry* e) { return e->st.key == key; });
    if (pos == list.end()) return;
    ptrdiff_t i = pos - list.begin(), j = i + dir;
    if (j < 0 || j >= (ptrdiff_t)list.size()) return;
    std::swap(list[i], list[j]);
    std::vector<std::wstring> order;
    for (auto* e : list) order.push_back(e->st.key);
    for (auto& k : g_order)  // keep the places of devices that are not shown right now
        if (std::find(order.begin(), order.end(), k) == order.end()) order.push_back(k);
    g_order = order;
    SaveOrder();
    UpdateWidget();
}

void ShowMenu() {
    ApplyMenuTheme(g_lightTaskbar);
    HMENU m = CreatePopupMenu();
    std::vector<std::wstring> keys;  // device index -> device key; menu id ID_DEVICE_FIRST + index * DEV_STRIDE + command
    std::vector<TrayEntry*> all = OrderedEntries(false), shown = OrderedEntries(true);
    if (all.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"No devices with battery info found");
    else AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Point at a device to show, hide or move it");
    for (TrayEntry* e : all) {
        const std::wstring& k = e->st.key;
        UINT base = ID_DEVICE_FIRST + (UINT)keys.size() * DEV_STRIDE;
        auto pos = std::find(shown.begin(), shown.end(), e);
        bool canLeft = pos != shown.end() && pos != shown.begin();
        bool canRight = pos != shown.end() && pos + 1 != shown.end();
        HMENU sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING | (IsHidden(k) ? 0 : MF_CHECKED), base + DEV_TOGGLE,
                    g_wantWidget ? L"Show on the taskbar" : L"Show in the tray");
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(sub, MF_STRING | (canLeft ? 0 : MF_GRAYED), base + DEV_LEFT, g_wantWidget ? L"Move left" : L"Move up");
        AppendMenuW(sub, MF_STRING | (canRight ? 0 : MF_GRAYED), base + DEV_RIGHT, g_wantWidget ? L"Move right" : L"Move down");
        AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
        {
            HMENU lowMenu = CreatePopupMenu();
            auto own = g_deviceLow.find(k);
            UINT checked = own == g_deviceLow.end() ? base + DEV_LOW_DEFAULT : own->second == 0 ? base + DEV_LOW_NEVER : 0;
            AppendMenuW(lowMenu, MF_STRING, base + DEV_LOW_DEFAULT, (L"Default (" + std::to_wstring(g_lowDefault) + L"%)").c_str());
            AppendMenuW(lowMenu, MF_STRING, base + DEV_LOW_NEVER, L"Never");
            AppendMenuW(lowMenu, MF_SEPARATOR, 0, nullptr);
            for (size_t i = 0; i < std::size(kLowChoices); i++) {
                AppendMenuW(lowMenu, MF_STRING, base + DEV_LOW_FIRST + (UINT)i, (std::to_wstring(kLowChoices[i]) + L"%").c_str());
                if (own != g_deviceLow.end() && own->second == kLowChoices[i]) checked = base + DEV_LOW_FIRST + (UINT)i;
            }
            CheckMenuRadioItem(lowMenu, base + DEV_LOW_DEFAULT, base + DEV_LOW_FIRST + (UINT)std::size(kLowChoices) - 1, checked, MF_BYCOMMAND);
            AppendMenuW(sub, MF_POPUP, (UINT_PTR)lowMenu, L"Low battery warning at");
        }
        if (e->st.vid) {
            int sharing = 0;  // devices reported through the same receiver
            for (TrayEntry* o : all) sharing += o->st.vid == e->st.vid && o->st.pid == e->st.pid;
            std::wstring label = L"Never query " + UsbId(e->st.vid, e->st.pid) + (sharing > 1 ? L" (and the other devices on it)" : L"");
            AppendMenuW(sub, MF_STRING, base + DEV_IGNORE, label.c_str());
        }
        std::wstring line = e->st.name + L":  " + StatusText(*e) + L"   [" + e->st.via + L"]";
        AppendMenuW(m, MF_POPUP | (IsHidden(k) ? 0 : MF_CHECKED), (UINT_PTR)sub, line.c_str());
        keys.push_back(k);
    }
    AppendMenuW(m, MF_STRING | (g_hideLastKnown ? MF_CHECKED : 0), ID_HIDE_LAST_KNOWN, L"Hide last known / asleep devices");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (g_wantWidget ? MF_CHECKED : 0), ID_MODE_WIDGET, L"Show on the taskbar (icon + percentage)");
    AppendMenuW(m, MF_STRING | (!g_wantWidget ? MF_CHECKED : 0), ID_MODE_TRAY, L"Show as tray icons");
    CheckMenuRadioItem(m, ID_MODE_WIDGET, ID_MODE_TRAY, g_wantWidget ? ID_MODE_WIDGET : ID_MODE_TRAY, MF_BYCOMMAND);
    UINT bgFlags = MF_STRING | (g_widgetBackground ? MF_CHECKED : 0) | (g_wantWidget ? 0 : MF_GRAYED);
    AppendMenuW(m, bgFlags, ID_WIDGET_BACKGROUND, L"Background behind taskbar text (for transparent taskbars)");
    {
        HMENU settings = CreatePopupMenu();
        HMENU lowMenu = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kLowChoices); i++)
            AppendMenuW(lowMenu, MF_STRING, ID_LOW_FIRST + (UINT)i, (std::to_wstring(kLowChoices[i]) + L"%").c_str());
        size_t lowIdx = std::find(std::begin(kLowChoices), std::end(kLowChoices), g_lowDefault) - std::begin(kLowChoices);
        CheckMenuRadioItem(lowMenu, ID_LOW_FIRST, ID_LOW_FIRST + (UINT)std::size(kLowChoices) - 1, ID_LOW_FIRST + (UINT)lowIdx, MF_BYCOMMAND);
        AppendMenuW(settings, MF_POPUP, (UINT_PTR)lowMenu, L"Low battery warning at");
        AppendMenuW(settings, MF_STRING | (g_notifyLow ? MF_CHECKED : 0), ID_NOTIFY_LOW, L"Low battery notifications");
        AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
        HMENU every = CreatePopupMenu();
        const wchar_t* intervalNames[] = { L"30 seconds", L"1 minute", L"2 minutes", L"5 minutes", L"10 minutes" };
        static_assert(std::size(intervalNames) == std::size(kIntervalChoices), "one name per interval");
        for (size_t i = 0; i < std::size(kIntervalChoices); i++) AppendMenuW(every, MF_STRING, ID_INTERVAL_FIRST + (UINT)i, intervalNames[i]);
        size_t secIdx = std::find(std::begin(kIntervalChoices), std::end(kIntervalChoices), (int)(g_pollMs / 1000)) - std::begin(kIntervalChoices);
        CheckMenuRadioItem(every, ID_INTERVAL_FIRST, ID_INTERVAL_FIRST + (UINT)std::size(kIntervalChoices) - 1, ID_INTERVAL_FIRST + (UINT)secIdx,
                           MF_BYCOMMAND);
        AppendMenuW(settings, MF_POPUP, (UINT_PTR)every, L"Refresh every");
        AppendMenuW(settings, MF_STRING | (g_adaptive ? MF_CHECKED : 0), ID_ADAPTIVE, L"Ask devices less often while nothing changes");
        auto ignored = IgnoredDeviceIds();
        if (!ignored.empty()) {
            HMENU ign = CreatePopupMenu();
            AppendMenuW(ign, MF_STRING | MF_GRAYED, 0, L"Click one to query it again");
            for (size_t i = 0; i < ignored.size() && i < (size_t)kMaxIgnored; i++)
                AppendMenuW(ign, MF_STRING, ID_UNIGNORE_FIRST + (UINT)i, UsbId(ignored[i].first, ignored[i].second).c_str());
            AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(settings, MF_POPUP, (UINT_PTR)ign, L"Devices never queried");
        }
        AppendMenuW(m, MF_POPUP, (UINT_PTR)settings, L"Settings");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_REFRESH, L"Refresh now");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING, ID_SUPPORT, L"Support the project...");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    switch (cmd) {
        case ID_REFRESH: SetEvent(g_refreshEvt); break;
        case ID_AUTOSTART: SetAutostart(!AutostartEnabled()); break;
        case ID_HIDE_LAST_KNOWN: SetHideLastKnown(!g_hideLastKnown); break;
        case ID_WIDGET_BACKGROUND: SetWidgetBackground(!g_widgetBackground); break;
        case ID_MODE_WIDGET: SetMode(true); break;
        case ID_MODE_TRAY: SetMode(false); break;
        case ID_SUPPORT: ShellExecuteW(nullptr, L"open", L"https://github.com/sponsors/krisgfx9", nullptr, nullptr, SW_SHOWNORMAL); break;
        case ID_EXIT: DestroyWindow(g_hwnd); break;
        case ID_NOTIFY_LOW: SetNotifyLow(!g_notifyLow); break;
        case ID_ADAPTIVE: SetAdaptive(!g_adaptive); break;
        default:
            if (cmd >= ID_LOW_FIRST && cmd < ID_LOW_FIRST + std::size(kLowChoices)) {
                SetLowDefault(kLowChoices[cmd - ID_LOW_FIRST]);
            } else if (cmd >= ID_INTERVAL_FIRST && cmd < ID_INTERVAL_FIRST + std::size(kIntervalChoices)) {
                SetInterval(kIntervalChoices[cmd - ID_INTERVAL_FIRST]);
            } else if (cmd >= ID_UNIGNORE_FIRST && cmd < ID_UNIGNORE_FIRST + (UINT)kMaxIgnored) {
                UnignoreDevice(cmd - ID_UNIGNORE_FIRST);
            } else if (cmd >= ID_DEVICE_FIRST && cmd < ID_DEVICE_FIRST + keys.size() * DEV_STRIDE) {
                std::wstring key = keys[(cmd - ID_DEVICE_FIRST) / DEV_STRIDE];
                UINT sub = (cmd - ID_DEVICE_FIRST) % DEV_STRIDE;
                switch (sub) {
                    case DEV_TOGGLE: ToggleHidden(key); break;
                    case DEV_LEFT:   MoveEntry(key, -1); break;
                    case DEV_RIGHT:  MoveEntry(key, +1); break;
                    case DEV_LOW_DEFAULT: SetDeviceLow(key, -1); break;
                    case DEV_LOW_NEVER:   SetDeviceLow(key, 0); break;
                    case DEV_IGNORE: {
                        auto it = g_entries.find(key);
                        if (it != g_entries.end() && it->second.st.vid) IgnoreDevice(it->second.st.vid, it->second.st.pid);
                        break;
                    }
                    default:
                        if (sub >= DEV_LOW_FIRST && sub < DEV_LOW_FIRST + std::size(kLowChoices)) SetDeviceLow(key, kLowChoices[sub - DEV_LOW_FIRST]);
                }
            }
    }
}

// ---- worker thread ----------------------------------------------------------------------------

PollOptions WorkerOptions(bool force) {
    PollOptions o;
    o.force = force;
    o.nowMs = GetTickCount64();
    o.baseIntervalMs = g_pollMs;
    o.maxIntervalMs = std::max<uint64_t>(5 * 60 * 1000, g_pollMs);
    o.adaptive = g_adaptive;
    o.parallel = g_parallel;
    return o;
}

DWORD WINAPI Worker(LPVOID) {
    PollScheduler scheduler(AllDrivers());  // lives as long as the thread: remembers results between polls
    bool force = true;                      // the first poll asks every device
    for (;;) {
        auto* v = new std::vector<DeviceStatus>(PollAllDevices(scheduler, WorkerOptions(force)));
        if (!PostMessageW(g_hwnd, WM_RESULTS, 0, (LPARAM)v)) delete v;
        HANDLE hs[2] = { g_stopEvt, g_refreshEvt };
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, g_pollMs);
        if (r == WAIT_OBJECT_0) return 0;
        force = r == WAIT_OBJECT_0 + 1;  // Refresh now, a device plugged in or removed, resume: ask everything
    }
}

void RefreshSoon(UINT ms) { SetTimer(g_hwnd, TIMER_DEBOUNCE, ms, nullptr); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_taskbarCreatedMsg && msg != 0) {  // Explorer restarted
        widget::Destroy();
        g_useWidget = false;
        ReAddAll();
        Reconcile();
        return 0;
    }
    switch (msg) {
        case WM_RESULTS: {
            auto* v = reinterpret_cast<std::vector<DeviceStatus>*>(lp);
            OnResults(*v);
            delete v;
            return 0;
        }
        case WM_TRAY:
            switch (LOWORD(lp)) {
                case WM_CONTEXTMENU:
                case NIN_SELECT:
                case NIN_KEYSELECT: ShowMenu(); break;
            }
            return 0;
        case WM_WIDGET_CLICK:
            ShowMenu();
            return 0;
        case WM_ENDSESSION:  // sign-out / shutdown / installer restart manager
            if (wp) DestroyWindow(h);
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE) RefreshSoon(2500);
            return TRUE;
        case WM_POWERBROADCAST:
            if (wp == PBT_APMRESUMEAUTOMATIC) RefreshSoon(5000);
            return TRUE;
        case WM_TIMER:
            if (wp == TIMER_DEBOUNCE) { KillTimer(h, TIMER_DEBOUNCE); SetEvent(g_refreshEvt); }
            if (wp == TIMER_WIDGET) Reconcile();
            return 0;
        case WM_SETTINGCHANGE:
            if (lp && !lstrcmpW((LPCWSTR)lp, L"ImmersiveColorSet")) {
                bool light = ReadLightTaskbar();
                if (light != g_lightTaskbar) { g_lightTaskbar = light; ApplyMenuTheme(light); RedrawAll(); widget::SetLightTheme(light); }
            }
            return 0;
        case WM_DESTROY:
            KillTimer(h, TIMER_WIDGET);
            widget::Destroy();
            SetEvent(g_stopEvt);
            if (g_worker) WaitForSingleObject(g_worker, 5000);
            for (auto& [k, e] : g_entries) {
                if (e.added) TrayDelete(e.uid);
                if (e.icon) DestroyIcon(e.icon);
            }
            if (g_placeholder) TrayDelete(PLACEHOLDER_ID);
            if (g_notice) TrayDelete(NOTICE_ID);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// `WirelessBattery.exe --list` prints what it finds, `--dump` every HID collection. With --list:
//   --json        machine-readable output (json_report.h)
//   --timing      how long each driver took, and whether drivers ran in parallel
//   --sequential  run the drivers one after another (as before parallel polling)
struct ListFlags {
    bool dump = false, json = false, timing = false, sequential = false, taskbars = false;
};

int ListToConsole(const ListFlags& flags) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    auto write = [&](const std::wstring& s) {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, 0);
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), u.data(), n, nullptr, nullptr);
        DWORD w;
        WriteFile(out, u.data(), (DWORD)u.size(), &w, nullptr);
    };
    if (flags.dump) {
        write(DumpHidCollections());
        return 0;
    }
    if (flags.taskbars) {
        write(widget::DescribeTaskbars());
        return 0;
    }
    PollScheduler scheduler(AllDrivers());
    PollOptions opt;
    opt.force = true;
    opt.parallel = !flags.sequential && LoadBoolSetting(L"ParallelPolling", true);
    auto list = PollAllDevices(scheduler, opt);
    const PollReport& report = scheduler.Report();
    std::vector<int64_t> left;
    for (auto& s : list) left.push_back(s.charging || s.percent < 0 ? -1 : LoadTrend(s.key).RemainingSec(WallClock()));
    if (flags.json) {
        std::string j = DevicesToJson(list, left);
        DWORD w;
        WriteFile(out, j.data(), (DWORD)j.size(), &w, nullptr);
        return 0;
    }
    if (list.empty()) write(L"No devices with battery info found.\n");
    for (size_t i = 0; i < list.size(); i++) {
        auto& s = list[i];
        std::wstring line = s.name + L"  [" + s.via + L"]  ";
        if (!s.connected) line += L"off / not connected";
        else if (s.asleep) line += L"asleep";
        else if (!s.levelText.empty()) line += s.levelText;
        else if (s.percent >= 0) line += std::to_wstring(s.percent) + L"%";
        else line += L"unknown";
        if (!s.detail.empty()) line += L" (" + s.detail + L")";
        if (s.charging) line += L", charging";
        if (s.millivolts) line += L", " + std::to_wstring(s.millivolts) + L" mV";
        std::wstring est = FormatRemaining(left[i]);
        if (!est.empty()) line += L", " + est + L" left";
        write(line + L"   (" + s.key + L")\n");
    }
    auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };  // ASCII
    for (const auto& v : report.violations) write(L"warning: " + widen(v) + L" (please report this)\n");
    if (flags.timing) {
        wchar_t line[160];
        swprintf(line, 160, L"\n%ls, %d thread(s), %.0f ms in total\n", report.parallel ? L"parallel" : L"one after another", report.threads,
                 report.totalMs);
        write(line);
        for (const auto& t : report.timings) {
            if (t.ms < 1 && !t.devices) continue;  // drivers that found nothing to talk to
            swprintf(line, 160, L"  %-18ls %7.0f ms  %d device(s)\n", widen(t.name).c_str(), t.ms, t.devices);
            write(line);
        }
    }
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (cmdLine && (wcsstr(cmdLine, L"--list") || wcsstr(cmdLine, L"--dump") || wcsstr(cmdLine, L"--json") || wcsstr(cmdLine, L"--taskbars"))) {
        ListFlags f;
        f.taskbars = wcsstr(cmdLine, L"--taskbars") != nullptr;
        f.dump = wcsstr(cmdLine, L"--dump") != nullptr;
        f.json = wcsstr(cmdLine, L"--json") != nullptr;
        f.timing = wcsstr(cmdLine, L"--timing") != nullptr;
        f.sequential = wcsstr(cmdLine, L"--sequential") != nullptr;
        return ListToConsole(f);
    }
    if (cmdLine && wcsstr(cmdLine, L"--quit")) {  // used by the installer before upgrading/uninstalling
        if (HWND w = FindWindowW(L"WirelessBatteryWnd", nullptr)) {
            PostMessageW(w, WM_CLOSE, 0, 0);
            for (int i = 0; i < 50 && FindWindowW(L"WirelessBatteryWnd", nullptr); i++) Sleep(100);
        }
        return 0;
    }
    g_inst = inst;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WirelessBatteryTray");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    g_lightTaskbar = ReadLightTaskbar();
    ApplyMenuTheme(g_lightTaskbar);
    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"WirelessBatteryWnd";
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it receives the TaskbarCreated broadcast.
    g_hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{ sizeof(filter) };
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    g_refreshEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wantWidget = LoadWidgetMode();
    g_hideLastKnown = LoadHideLastKnown();
    g_widgetBackground = LoadWidgetBackground();
    LoadSettings();
    widget::SetBackground(g_widgetBackground);  // remembered by the widget across re-creation
    LoadHidden();
    LoadOrder();
    Reconcile();
    UpdatePlaceholder();  // something to click on until the first poll finishes
    SetTimer(g_hwnd, TIMER_WIDGET, 1000, nullptr);
    g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    // drain results posted after shutdown started
    while (PeekMessageW(&m, nullptr, WM_RESULTS, WM_RESULTS, PM_REMOVE)) delete reinterpret_cast<std::vector<DeviceStatus>*>(m.lParam);
    if (notify) UnregisterDeviceNotification(notify);
    if (g_worker) CloseHandle(g_worker);
    ReleaseMutex(mutex);
    return 0;
}
