#include "widget.h"

#include <commctrl.h>
#include <uiautomation.h>
#include <algorithm>

namespace {

// One widget per taskbar: the primary one (Shell_TrayWnd) and every secondary one
// (Shell_SecondaryTrayWnd, one per extra monitor when the taskbar is shown on all displays).
struct Bar {
    HWND taskbar = nullptr, wnd = nullptr, tip = nullptr;
    HWND rebar = nullptr;   // Windows 10 style task button strip that has to be shortened (primary only)
    bool primary = false;
    bool vertical = false;  // taskbar docked to the left or right screen edge
    bool hover = false;
    SIZE size{};
    RECT lastAnchor{}, lastBar{}, lastWidgets{};
    int lastLen = -1, lastDpi = 0, lastPos = -1;
};
std::vector<Bar*> g_bars;

HWND g_notify;
UINT g_clickMsg;
bool g_light;
bool g_background;  // opaque rounded plate behind the text, for transparent / restyled taskbars
std::vector<WidgetItem> g_items;
std::wstring g_tipText;
HINSTANCE g_inst;

const wchar_t* kClass = L"WirelessBatteryWidget";

int Dpi(const Bar& b) {
    UINT d = b.taskbar ? GetDpiForWindow(b.taskbar) : 0;
    return d ? (int)d : 96;
}

// Windows 11 puts the Widgets (weather) button right next to the tray when the taskbar is
// left-aligned (start-aligned on right-to-left Windows). It is XAML, so only UI Automation knows
// where it is. The element is looked up once per taskbar; its rectangle is re-read every few
// seconds because the weather text changes its width.
//
// UI Automation calls go to Explorer and wait for its UI thread, while our widget is a child window
// of the taskbar, so Explorer in turn waits for our UI thread whenever it lays out or sends input to
// the widget. Making those calls from our UI thread deadlocks both: Explorer is reported hung
// ("Cross-process", waiting on WirelessBattery.exe) and restarted, again and again. So the lookup
// runs on its own thread, which owns no windows, and the UI thread only reads the last rectangle.
SRWLOCK g_wbLock = SRWLOCK_INIT;
RECT g_wbRect{};             // last Widgets button rectangle (screen), empty when unknown
HWND g_wbRectBar;            // taskbar g_wbRect was found in (guarded by g_wbLock)
HWND g_wbTaskbar;            // taskbar to search, null while there is no widget (guarded by g_wbLock)
HANDLE g_wbWake;             // auto-reset: wakes the lookup thread early

DWORD WINAPI WidgetsButtonThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    IUIAutomationElement* btn = nullptr;
    HWND btnBar = nullptr;
    for (;;) {
        WaitForSingleObject(g_wbWake, btnBar ? 5000 : INFINITE);  // no widget (tray-icon mode): sleep until one
        AcquireSRWLockShared(&g_wbLock);
        HWND bar = g_wbTaskbar;
        ReleaseSRWLockShared(&g_wbLock);
        if (btn && bar != btnBar) { btn->Release(); btn = nullptr; }
        btnBar = bar;
        RECT r{};
        if (bar && IsWindow(bar)) {
            if (!uia)
                CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation), (void**)&uia);
            if (uia && !btn) {
                IUIAutomationElement* root = nullptr;
                IUIAutomationCondition* cond = nullptr;
                VARIANT id;
                VariantInit(&id);
                id.vt = VT_BSTR;
                id.bstrVal = SysAllocString(L"WidgetsButton");
                if (SUCCEEDED(uia->ElementFromHandle(bar, &root)) && root &&
                    SUCCEEDED(uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &cond)) && cond)
                    root->FindFirst(TreeScope_Descendants, cond, &btn);
                VariantClear(&id);
                if (cond) cond->Release();
                if (root) root->Release();
            }
            if (btn && FAILED(btn->get_CurrentBoundingRectangle(&r))) {
                btn->Release();  // stale (Widgets turned off, Explorer restarted): look it up again next time
                btn = nullptr;
                r = {};
            }
        }
        AcquireSRWLockExclusive(&g_wbLock);
        if (bar && g_wbTaskbar == bar) {  // drop a result for a taskbar we no longer sit on
            g_wbRect = r;
            g_wbRectBar = bar;
        }
        ReleaseSRWLockExclusive(&g_wbLock);
    }
}

// Points the lookup thread at `taskbar` (null to idle it), starting the thread on first use.
// The last rectangle is kept for its taskbar, so switching from tray-icon back to taskbar mode on the
// same taskbar steps past the Widgets button straight away instead of after the next lookup.
void WatchWidgetsButton(HWND taskbar) {
    AcquireSRWLockExclusive(&g_wbLock);
    g_wbTaskbar = taskbar;
    ReleaseSRWLockExclusive(&g_wbLock);
    if (!g_wbWake) {
        g_wbWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE t = g_wbWake ? CreateThread(nullptr, 0, WidgetsButtonThread, nullptr, 0, nullptr) : nullptr;
        if (t) CloseHandle(t);
    }
    if (g_wbWake) SetEvent(g_wbWake);
}

// Never blocks: returns what the lookup thread last found on the current taskbar.
RECT WidgetsButtonRect() {
    AcquireSRWLockShared(&g_wbLock);
    RECT r = g_wbRectBar && g_wbRectBar == g_wbTaskbar ? g_wbRect : RECT{};
    ReleaseSRWLockShared(&g_wbLock);
    return r;
}

wchar_t Glyph(DeviceKind k, bool bluetooth) {
    switch (k) {
        case DeviceKind::Headset:  return 0xE7F6;  // Headphone
        case DeviceKind::Mouse:    return 0xE962;  // Mouse
        case DeviceKind::Keyboard: return 0xE765;  // KeyboardClassic
        case DeviceKind::Gamepad:  return 0xE7FC;  // Game
        case DeviceKind::Phone:    return 0xE8EA;  // CellPhone
        default:                   return bluetooth ? 0xE702 : 0xE772;  // Bluetooth / Devices
    }
}
constexpr wchar_t kBolt = 0xE945;            // LightningBolt
constexpr wchar_t kBatteryUnknown = 0xE996;  // shown when there is nothing to display

// Creates a font, falling back when the preferred face is not installed (GDI would silently substitute).
HFONT MakeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback, int fallbackPx = 0) {
    for (const wchar_t* f : { face, fallback }) {
        int h_px = (f == fallback && fallbackPx > 0) ? fallbackPx : px;
        HFONT h = CreateFontW(-h_px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, f);
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ o = SelectObject(dc, h);
        wchar_t got[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, got);
        SelectObject(dc, o);
        DeleteDC(dc);
        if (!_wcsicmp(got, f) || f == fallback) return h;
        DeleteObject(h);
    }
    return nullptr;
}

// Grayscale coverage mask of a string, cropped to its ink bounds.
struct Mask {
    int w = 0, h = 0;
    std::vector<BYTE> a;
};

Mask RenderMask(const std::wstring& s, HFONT font, bool darkOnLight) {
    Mask m;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ of = SelectObject(dc, font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    int W = sz.cx + 8, H = sz.cy + 8;
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, darkOnLight ? 0xFF : 0x00, (size_t)W * H * 4);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, darkOnLight ? RGB(0, 0, 0) : RGB(255, 255, 255));
    TextOutW(dc, 4, 4, s.c_str(), (int)s.size());
    GdiFlush();
    auto* p = static_cast<DWORD*>(bits);
    auto cover = [&](DWORD v) -> BYTE {
        int c = (int)((v & 0xFF) + ((v >> 8) & 0xFF) + ((v >> 16) & 0xFF)) / 3;
        return (BYTE)(darkOnLight ? 255 - c : c);
    };
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (cover(p[y * W + x])) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
    if (x1 >= 0) {
        m.w = x1 - x0 + 1;
        m.h = y1 - y0 + 1;
        m.a.resize((size_t)m.w * m.h);
        for (int y = 0; y < m.h; y++)
            for (int x = 0; x < m.w; x++) m.a[y * m.w + x] = cover(p[(y0 + y) * W + x0 + x]);
    }
    SelectObject(dc, ob);
    SelectObject(dc, of);
    DeleteObject(bmp);
    DeleteDC(dc);
    return m;
}

struct Rgb { int r, g, b; };

struct Canvas {
    int w, h;
    std::vector<DWORD> px;  // premultiplied BGRA
    Canvas(int w_, int h_) : w(w_), h(h_), px((size_t)w_ * h_, 0) {}
    void Blend(int x, int y, Rgb c, int a) {  // source-over, a = 0..255
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        DWORD d = px[y * w + x];
        int inv = 255 - a;
        int da = d >> 24, dr = (d >> 16) & 255, dg = (d >> 8) & 255, db = d & 255;
        int oa = a + da * inv / 255;
        int orr = c.r * a / 255 + dr * inv / 255;
        int og = c.g * a / 255 + dg * inv / 255;
        int ob = c.b * a / 255 + db * inv / 255;
        px[y * w + x] = ((DWORD)oa << 24) | ((DWORD)orr << 16) | ((DWORD)og << 8) | (DWORD)ob;
    }
    void Draw(const Mask& m, int x, int y, Rgb c, int alpha) {
        for (int j = 0; j < m.h; j++)
            for (int i = 0; i < m.w; i++) Blend(x + i, y + j, c, m.a[j * m.w + i] * alpha / 255);
    }
};

// A glyph or a piece of text, and where it goes in the widget.
struct Placed {
    Mask m;
    Rgb color;
    int alpha;
    int x, y;
};

struct Fonts {
    HFONT icons, bolt, text;
    explicit Fonts(const Bar& b) {
        int dpi = Dpi(b);
        auto px = [dpi](int v) { return MulDiv(v, dpi, 96); };
        icons = MakeFont(px(16), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
        bolt = MakeFont(px(11), FW_NORMAL, L"Segoe Fluent Icons", L"Segoe MDL2 Assets");
        text = MakeFont(px(13), FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI", px(12));
    }
    ~Fonts() {
        DeleteObject(icons);
        DeleteObject(bolt);
        DeleteObject(text);
    }
};

// One device: its glyph, its level and the charging bolt.
struct ItemMasks {
    Mask glyph, text, bolt;
    Rgb textColor;
    int alpha;
    bool charging;
};

std::vector<ItemMasks> RenderItems(const Fonts& f) {
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb red = g_light ? Rgb{ 196, 30, 40 } : Rgb{ 255, 90, 90 };
    std::vector<ItemMasks> out;
    for (const auto& it : g_items) {
        ItemMasks m;
        m.alpha = it.dim ? 120 : 255;
        m.textColor = it.low ? red : fg;
        m.charging = it.charging;
        m.glyph = RenderMask(std::wstring(1, Glyph(it.kind, it.bluetooth)), f.icons, g_light);
        std::wstring t = !it.text.empty() ? it.text : it.percent >= 0 ? std::to_wstring(it.percent) + L"%" : L"?";
        m.text = RenderMask(t, f.text, g_light);
        if (it.charging) m.bolt = RenderMask(std::wstring(1, kBolt), f.bolt, g_light);
        out.push_back(std::move(m));
    }
    return out;
}

// Horizontal taskbar: one row, "[icon] 30%  [icon] 93%", as tall as the taskbar.
std::vector<Placed> LayoutRow(const Bar& b, int H, int& W) {
    auto Px = [&](int v) { return MulDiv(v, Dpi(b), 96); };
    Fonts f(b);
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    std::vector<Placed> out;
    int x = Px(8);
    auto put = [&](Mask m, Rgb c, int a, int gapAfter) {
        int w = m.w, h = m.h;
        out.push_back({ std::move(m), c, a, x, (H - h) / 2 });
        x += w + gapAfter;
    };
    if (g_items.empty()) put(RenderMask(std::wstring(1, kBatteryUnknown), f.icons, g_light), fg, 110, 0);
    auto items = RenderItems(f);
    for (size_t i = 0; i < items.size(); i++) {
        auto& it = items[i];
        int gapEnd = i + 1 < items.size() ? Px(14) : 0;
        put(std::move(it.glyph), fg, it.alpha, Px(5));
        put(std::move(it.text), it.textColor, it.alpha, it.charging ? Px(2) : gapEnd);
        if (it.charging) put(std::move(it.bolt), charge, it.alpha, gapEnd);
    }
    W = x + Px(8);
    return out;
}

// Vertical taskbar: one device per row, centred; when "[icon] 93%" is wider than the taskbar, the
// level goes under the icon.
std::vector<Placed> LayoutColumn(const Bar& b, int W, int& H) {
    auto Px = [&](int v) { return MulDiv(v, Dpi(b), 96); };
    Fonts f(b);
    Rgb fg = g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 };
    Rgb charge = g_light ? Rgb{ 0, 120, 212 } : Rgb{ 96, 205, 255 };
    std::vector<Placed> out;
    int y = Px(6);
    auto items = RenderItems(f);
    if (items.empty()) {
        Mask m = RenderMask(std::wstring(1, kBatteryUnknown), f.icons, g_light);
        int h = m.h, w = m.w;
        out.push_back({ std::move(m), fg, 110, (W - w) / 2, y });
        y += h;
    }
    for (size_t i = 0; i < items.size(); i++) {
        auto& it = items[i];
        int levelW = it.text.w + (it.charging ? Px(2) + it.bolt.w : 0);
        int levelH = std::max(it.text.h, it.charging ? it.bolt.h : 0);
        int rowW = it.glyph.w + Px(5) + levelW;
        if (rowW + Px(4) * 2 <= W) {  // one line
            int rowH = std::max(it.glyph.h, levelH);
            int x = (W - rowW) / 2;
            int gw = it.glyph.w, gh = it.glyph.h, tw = it.text.w, th = it.text.h, bh = it.bolt.h;
            out.push_back({ std::move(it.glyph), fg, it.alpha, x, y + (rowH - gh) / 2 });
            x += gw + Px(5);
            out.push_back({ std::move(it.text), it.textColor, it.alpha, x, y + (rowH - th) / 2 });
            if (it.charging) out.push_back({ std::move(it.bolt), charge, it.alpha, x + tw + Px(2), y + (rowH - bh) / 2 });
            y += rowH;
        } else {  // icon above the level
            int gw = it.glyph.w, gh = it.glyph.h, tw = it.text.w, th = it.text.h, bh = it.bolt.h;
            out.push_back({ std::move(it.glyph), fg, it.alpha, (W - gw) / 2, y });
            y += gh + Px(3);
            int x = (W - levelW) / 2;
            out.push_back({ std::move(it.text), it.textColor, it.alpha, x, y + (levelH - th) / 2 });
            if (it.charging) out.push_back({ std::move(it.bolt), charge, it.alpha, x + tw + Px(2), y + (levelH - bh) / 2 });
            y += levelH;
        }
        if (i + 1 < items.size()) y += Px(10);
    }
    H = y + Px(6);
    return out;
}

void Paint(Bar& b) {
    if (!b.wnd) return;
    auto Px = [&](int v) { return MulDiv(v, Dpi(b), 96); };
    RECT client;
    GetClientRect(b.taskbar, &client);
    int W = 0, H = 0;
    std::vector<Placed> pieces;
    if (b.vertical) {
        W = client.right - client.left;
        pieces = LayoutColumn(b, W, H);
    } else {
        H = client.bottom - client.top;
        pieces = LayoutRow(b, H, W);
    }
    if (W <= 0 || H <= 0) return;
    Canvas cv(W, H);

    // Nearly transparent background so the whole strip receives mouse input,
    // plus a rounded hover highlight like the system tray icons have.
    for (auto& p : cv.px) p = 0x01000000;
    // Rounded rectangle, inset across the taskbar (top/bottom on a horizontal one, left/right on a vertical one).
    auto roundRect = [&](int inset, int r, Rgb c, int a) {
        int ix = b.vertical ? inset : 0, iy = b.vertical ? 0 : inset;
        for (int y = iy; y < H - iy; y++)
            for (int x = ix; x < W - ix; x++) {
                int dx = x < ix + r ? ix + r - x : (x >= W - ix - r ? x - (W - ix - r - 1) : 0);
                int dy = y < iy + r ? iy + r - y : (y >= H - iy - r ? y - (H - iy - r - 1) : 0);
                if (dx * dx + dy * dy <= r * r) cv.Blend(x, y, c, a);
            }
    };
    // Optional plate in the taskbar's own colours: keeps the text readable when a taskbar theme
    // (e.g. Windhawk's Taskbar Styler) makes the bar transparent over the wallpaper.
    if (g_background) roundRect(Px(4), Px(6), g_light ? Rgb{ 243, 243, 243 } : Rgb{ 32, 32, 32 }, 240);
    if (b.hover) roundRect(Px(4), Px(4), g_light ? Rgb{ 0, 0, 0 } : Rgb{ 255, 255, 255 }, g_light ? 20 : 26);

    for (auto& p : pieces) cv.Draw(p.m, p.x, p.y, p.color, p.alpha);

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    memcpy(bits, cv.px.data(), (size_t)W * H * 4);
    HGDIOBJ ob = SelectObject(mem, bmp);
    b.size = { W, H };
    SIZE sz{ W, H };
    POINT src{ 0, 0 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(b.wnd, screen, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, ob);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    b.lastLen = -1;  // force a re-position with the new size
}

bool IsVertical(HWND taskbar) {
    RECT r;
    GetWindowRect(taskbar, &r);
    return r.bottom - r.top > r.right - r.left;
}

// What the widget sits in front of, in screen coordinates: the notification area on the primary
// taskbar; on a secondary one, the run of small windows at its far end (clock, tray icons) that
// taskbar mods such as StartAllBack put there. Empty when there is nothing to sit next to.
RECT Anchor(const Bar& b) {
    RECT none{};
    if (HWND tray = FindWindowExW(b.taskbar, nullptr, L"TrayNotifyWnd", nullptr)) {
        RECT r;
        GetWindowRect(tray, &r);
        return r;
    }
    if (b.primary) return none;
    RECT bar;
    GetWindowRect(b.taskbar, &bar);
    bool rtl = GetWindowLongW(b.taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
    int len = b.vertical ? bar.bottom - bar.top : bar.right - bar.left;
    int slack = MulDiv(8, Dpi(b), 96);
    std::vector<RECT> parts;
    for (HWND c = GetWindow(b.taskbar, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c)) continue;
        RECT r;
        GetWindowRect(c, &r);
        int along = b.vertical ? r.bottom - r.top : r.right - r.left;
        if (along <= 0 || along * 2 > len || IsRectEmpty(&r)) continue;  // the task list spans most of the bar
        parts.push_back(r);
    }
    // Start from the part that touches the far end (right / bottom; left on right-to-left Windows) and
    // take in every part that adjoins the run.
    auto start = [&](const RECT& r) { return b.vertical ? r.top : rtl ? -r.right : r.left; };
    auto end = [&](const RECT& r) { return b.vertical ? r.bottom : rtl ? -r.left : r.right; };
    int farEnd = b.vertical ? bar.bottom : rtl ? -bar.left : bar.right;
    RECT run{};
    bool have = false;
    for (bool grew = true; grew;) {
        grew = false;
        for (auto it = parts.begin(); it != parts.end(); ++it) {
            bool joins = have ? end(*it) >= start(run) - slack && start(*it) < start(run) : end(*it) >= farEnd - slack;
            if (!joins) continue;
            if (have) UnionRect(&run, &run, &*it);
            else run = *it;
            have = true;
            parts.erase(it);
            grew = true;
            break;
        }
    }
    return have ? run : none;
}

bool Reposition(Bar& b);

LRESULT CALLBACK WidgetProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Bar* b = reinterpret_cast<Bar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_MOUSEMOVE:
            if (b && !b->hover) {
                b->hover = true;
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
                TrackMouseEvent(&t);
                Paint(*b);
                Reposition(*b);
            }
            return 0;
        case WM_MOUSELEAVE:
            if (b) {
                b->hover = false;
                Paint(*b);
                Reposition(*b);
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            PostMessageW(g_notify, g_clickMsg, 0, 0);
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void SetTip(Bar& b) {
    if (!b.tip) return;
    TTTOOLINFOW ti{ sizeof(ti) };
    ti.hwnd = g_notify;
    ti.uId = (UINT_PTR)b.wnd;
    ti.lpszText = const_cast<wchar_t*>(g_tipText.empty() ? L"Wireless Battery" : g_tipText.c_str());
    SendMessageW(b.tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

void ApplyTipTheme(Bar& b) {
    if (!b.tip) return;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static auto setTheme = ux ? (SetWindowThemeFn)GetProcAddress(ux, "SetWindowTheme") : nullptr;
    if (setTheme) {
        setTheme(b.tip, g_light ? L"Explorer" : L"DarkMode_Explorer", nullptr);
        SendMessageW(b.tip, WM_THEMECHANGED, 0, 0);
    }
}

Bar* CreateBar(HWND taskbar, bool primary) {
    // A secondary taskbar gets a widget only when there is something to sit next to (Windows 11's own
    // secondary taskbar draws its clock in XAML, with no window to find, and would end up covered).
    auto b = new Bar;
    b->taskbar = taskbar;
    b->primary = primary;
    b->vertical = IsVertical(taskbar);
    if (primary) {
        if (!FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr)) { delete b; return nullptr; }
        // Windows 11's taskbar is a XAML island; on Windows 10 the task buttons live in ReBarWindow32
        // and have to be shortened so they never slide under the widget.
        b->rebar = FindWindowExW(taskbar, nullptr, L"Windows.UI.Composition.DesktopWindowContentBridge", nullptr)
                       ? nullptr
                       : FindWindowExW(taskbar, nullptr, L"ReBarWindow32", nullptr);
    } else {
        RECT a = Anchor(*b);
        if (IsRectEmpty(&a)) { delete b; return nullptr; }
    }
    b->wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, taskbar, nullptr,
                             g_inst, nullptr);
    if (!b->wnd) { delete b; return nullptr; }
    SetWindowLongPtrW(b->wnd, GWLP_USERDATA, (LONG_PTR)b);

    b->tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, g_notify,
                             nullptr, g_inst, nullptr);
    if (b->tip) {
        ApplyTipTheme(*b);
        TTTOOLINFOW ti{ sizeof(ti) };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_notify;
        ti.uId = (UINT_PTR)b->wnd;
        ti.lpszText = const_cast<wchar_t*>(L"Wireless Battery");
        SendMessageW(b->tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(b->tip, TTM_SETMAXTIPWIDTH, 0, 600);
        SetTip(*b);
    }
    b->lastDpi = Dpi(*b);
    Paint(*b);
    Reposition(*b);
    ShowWindow(b->wnd, SW_SHOWNOACTIVATE);
    return b;
}

void DestroyBar(Bar* b) {
    if (b->rebar && IsWindow(b->rebar) && IsWindow(b->taskbar)) {
        // Let Explorer lay the task buttons out again over the full length.
        RECT r;
        GetClientRect(b->taskbar, &r);
        PostMessageW(b->taskbar, WM_SIZE, SIZE_RESTORED, MAKELPARAM(r.right, r.bottom));
    }
    if (b->tip) DestroyWindow(b->tip);
    if (b->wnd) DestroyWindow(b->wnd);
    delete b;
}

// The taskbars there are right now: the primary one first.
std::vector<HWND> Taskbars() {
    std::vector<HWND> v;
    if (HWND p = FindWindowW(L"Shell_TrayWnd", nullptr)) v.push_back(p);
    for (HWND s = nullptr; (s = FindWindowExW(nullptr, s, L"Shell_SecondaryTrayWnd", nullptr));) v.push_back(s);
    return v;
}

bool Reposition(Bar& b) {
    if (!IsWindow(b.wnd) || !IsWindow(b.taskbar)) return false;
    if (IsVertical(b.taskbar) != b.vertical) return false;  // taskbar moved to another edge: start over
    auto Px = [&](int v) { return MulDiv(v, Dpi(b), 96); };
    if (Dpi(b) != b.lastDpi) { b.lastDpi = Dpi(b); Paint(b); }
    RECT anchor = Anchor(b);
    if (IsRectEmpty(&anchor)) return true;  // nothing to sit next to right now: keep the last place
    RECT bar;
    GetWindowRect(b.taskbar, &bar);
    RECT wb{};
    int pos;  // client x (horizontal) or y (vertical) of the widget

    // Optional user override in DPI-independent pixels (e.g. for custom padding).
    DWORD customOffset = 0, sz = sizeof(customOffset);
    bool hasOffset = RegGetValueW(HKEY_CURRENT_USER, L"Software\\WirelessBattery", L"TaskbarOffset", RRF_RT_REG_DWORD, nullptr,
                                  &customOffset, &sz) == ERROR_SUCCESS;

    if (!b.vertical) {
        // Right-to-left Windows (Hebrew, Arabic) mirrors the taskbar: client x runs from the screen's right
        // edge, so a window's client-space left edge is its screen-space right edge.
        bool rtl = GetWindowLongW(b.taskbar, GWL_EXSTYLE) & WS_EX_LAYOUTRTL;
        int edge = rtl ? anchor.right : anchor.left;  // screen x we sit next to
        if (b.primary) {
            // Windows 10: News and Interests / Weather sits immediately left of the tray. The window that hosts it
            // depends on the build: ShellFeedsTaskbarViewWindow on some, DynamicContent2 on others (e.g. 19045,
            // where the search highlights button is DynamicContent1). Both are direct children of the taskbar and
            // only a visible one counts. Its rectangle is read on every pass because the weather text changes its
            // width, so the widget follows it.
            if (!rtl)
                for (const wchar_t* cls : { L"ShellFeedsTaskbarViewWindow", L"DynamicContent2" }) {
                    HWND feeds = FindWindowExW(b.taskbar, nullptr, cls, nullptr);
                    if (!feeds || !IsWindowVisible(feeds)) continue;
                    RECT fr;
                    GetWindowRect(feeds, &fr);
                    if (fr.left < edge && fr.right > fr.left) edge = fr.left;
                }
            // Step past the Widgets button when it sits where we would go.
            wb = WidgetsButtonRect();
            if (wb.right > wb.left && wb.top < bar.bottom && wb.bottom > bar.top &&
                (rtl ? wb.left < edge + b.size.cx + Px(2) && wb.right > edge : wb.right > edge - b.size.cx - Px(2) && wb.left < edge))
                edge = rtl ? wb.right : wb.left;
        }
        POINT p{ edge, bar.top };
        ScreenToClient(b.taskbar, &p);
        pos = p.x - b.size.cx - Px(2);
        if (hasOffset) pos -= Px((int)customOffset);

        // Windows 10: Explorer re-expands the task buttons on its own layout passes; shorten them again.
        if (b.rebar && IsWindow(b.rebar)) {
            RECT rr;
            GetWindowRect(b.rebar, &rr);
            POINT tl{ rtl ? rr.right : rr.left, rr.top };
            ScreenToClient(b.taskbar, &tl);
            if (tl.x + (rr.right - rr.left) > pos && pos > tl.x) MoveWindow(b.rebar, tl.x, tl.y, pos - tl.x, rr.bottom - rr.top, TRUE);
        }
    } else {
        // Vertical: just above the notification area / clock.
        POINT p{ bar.left, anchor.top };
        ScreenToClient(b.taskbar, &p);
        pos = p.y - b.size.cy - Px(2);
        if (hasOffset) pos -= Px((int)customOffset);
        if (b.rebar && IsWindow(b.rebar)) {
            RECT rr;
            GetWindowRect(b.rebar, &rr);
            POINT tl{ rr.left, rr.top };
            ScreenToClient(b.taskbar, &tl);
            if (tl.y + (rr.bottom - rr.top) > pos && pos > tl.y) MoveWindow(b.rebar, tl.x, tl.y, rr.right - rr.left, pos - tl.y, TRUE);
        }
    }
    if (pos < 0) pos = 0;

    // Keep us above the taskbar's own content (our sibling windows).
    bool covered = GetWindow(b.wnd, GW_HWNDPREV) != nullptr;
    int len = b.vertical ? b.size.cy : b.size.cx;
    if (!covered && EqualRect(&anchor, &b.lastAnchor) && EqualRect(&bar, &b.lastBar) && EqualRect(&wb, &b.lastWidgets) &&
        b.lastLen == len && b.lastPos == pos)
        return true;
    b.lastAnchor = anchor;
    b.lastWidgets = wb;
    b.lastBar = bar;
    b.lastLen = len;
    b.lastPos = pos;
    SetWindowPos(b.wnd, HWND_TOP, b.vertical ? 0 : pos, b.vertical ? pos : 0, b.size.cx, b.size.cy, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return true;
}

}  // namespace

namespace widget {

bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme) {
    Destroy();
    g_inst = inst;
    g_notify = notifyWnd;
    g_clickMsg = clickMsg;
    g_light = lightTheme;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WidgetProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        registered = RegisterClassExW(&wc) != 0;
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
    }
    auto bars = Taskbars();
    if (bars.empty() || !FindWindowExW(bars[0], nullptr, L"TrayNotifyWnd", nullptr)) return false;  // Explorer not ready yet
    for (size_t i = 0; i < bars.size(); i++)
        if (Bar* b = CreateBar(bars[i], i == 0)) g_bars.push_back(b);
    if (g_bars.empty()) return false;
    // The Windows 11 Widgets button only exists on a horizontal primary taskbar.
    if (g_bars[0]->primary && !g_bars[0]->vertical) WatchWidgetsButton(g_bars[0]->taskbar);
    else if (g_wbWake) WatchWidgetsButton(nullptr);
    return true;
}

void Destroy() {
    for (Bar* b : g_bars) DestroyBar(b);
    g_bars.clear();
    if (g_wbWake) WatchWidgetsButton(nullptr);
}

bool Alive() {
    if (g_bars.empty()) return false;
    for (Bar* b : g_bars)
        if (!IsWindow(b->wnd) || !IsWindow(b->taskbar) || IsVertical(b->taskbar) != b->vertical) return false;
    return true;
}

void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip) {
    g_items = items;
    bool tipChanged = tooltip != g_tipText;
    g_tipText = tooltip;
    for (Bar* b : g_bars) {
        if (tipChanged) SetTip(*b);
        Paint(*b);
        Reposition(*b);
    }
}

void SetLightTheme(bool light) {
    g_light = light;
    for (Bar* b : g_bars) {
        ApplyTipTheme(*b);
        Paint(*b);
    }
}

void SetBackground(bool on) {
    g_background = on;
    for (Bar* b : g_bars) Paint(*b);
}

bool Reposition() {
    if (g_bars.empty()) return false;
    bool ok = true;
    for (Bar* b : g_bars) ok &= Reposition(*b);
    // A taskbar that appeared (monitor plugged in, "show the taskbar on all displays" turned on) or
    // that had nothing to sit next to yet (a taskbar mod still building it) gets its widget now.
    for (HWND tb : Taskbars()) {
        bool have = false;
        for (Bar* b : g_bars) have |= b->taskbar == tb;
        if (have) continue;
        bool primary = tb == FindWindowW(L"Shell_TrayWnd", nullptr);
        if (Bar* b = CreateBar(tb, primary)) g_bars.push_back(b);
    }
    return ok;
}

}  // namespace widget

namespace {

struct DescribeCtx {
    HWND root;
    std::wstring* out;
    int lines;
};

std::wstring WindowLine(HWND w, int depth) {
    wchar_t cls[128] = {}, text[64] = {};
    GetClassNameW(w, cls, 128);
    InternalGetWindowText(w, text, 64);  // does not send messages to Explorer
    RECT r{};
    GetWindowRect(w, &r);
    wchar_t line[400];
    swprintf(line, 400, L"%*ls%ls  [%ld,%ld - %ld,%ld  %ldx%ld]%ls%ls%ls%ls\n", depth * 2, L"", cls, r.left, r.top, r.right, r.bottom,
             r.right - r.left, r.bottom - r.top, IsWindowVisible(w) ? L"" : L"  hidden", text[0] ? L"  \"" : L"", text,
             text[0] ? L"\"" : L"");
    return line;
}

BOOL CALLBACK DescribeChild(HWND w, LPARAM lp) {
    auto* ctx = reinterpret_cast<DescribeCtx*>(lp);
    int depth = 0;
    for (HWND p = w; p && p != ctx->root; p = GetParent(p)) depth++;
    if (depth > 5 || ++ctx->lines > 300) return TRUE;
    *ctx->out += WindowLine(w, depth);
    return TRUE;
}

BOOL CALLBACK DescribeTop(HWND w, LPARAM lp) {
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"Shell_TrayWnd") && wcscmp(cls, L"Shell_SecondaryTrayWnd")) return TRUE;
    auto* out = reinterpret_cast<std::wstring*>(lp);
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT r{};
    GetWindowRect(w, &r);
    wchar_t head[200];
    swprintf(head, 200, L"\n=== %ls on monitor [%ld,%ld - %ld,%ld]%ls, %ls, dpi %u\n", cls, mi.rcMonitor.left, mi.rcMonitor.top,
             mi.rcMonitor.right, mi.rcMonitor.bottom, (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" (primary)" : L"",
             (r.bottom - r.top > r.right - r.left) ? L"vertical" : L"horizontal", GetDpiForWindow(w));
    *out += head;
    *out += WindowLine(w, 0);
    DescribeCtx ctx{ w, out, 0 };
    EnumChildWindows(w, DescribeChild, (LPARAM)&ctx);
    return TRUE;
}

}  // namespace

namespace widget {

std::wstring DescribeTaskbars() {
    std::wstring out = L"Taskbars (class  [left,top - right,bottom  width x height]):\n";
    EnumWindows(DescribeTop, (LPARAM)&out);
    return out;
}

}  // namespace widget
