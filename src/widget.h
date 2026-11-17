#pragma once
// A small transparent strip on every taskbar, just before the notification area, showing
// "[icon] 30%  [icon] 93%" (one device per row on a vertical taskbar). Implemented as layered child
// windows of the primary taskbar (Shell_TrayWnd) and of each secondary one (Shell_SecondaryTrayWnd)
// that has a clock or tray area to sit next to.
#include <windows.h>
#include <string>
#include <vector>
#include "devices.h"

struct WidgetItem {
    DeviceKind kind = DeviceKind::Other;
    int  percent  = -1;     // -1 = unknown
    std::wstring text;      // shown instead of the percentage when set (coarse levels like "Low")
    bool charging = false;
    bool dim      = false;  // asleep / stale value
    bool low      = false;
    bool bluetooth = false;  // picks the Bluetooth icon when the device type is unknown
};

namespace widget {
// Creates the widgets. Clicks are reported by posting `clickMsg` to `notifyWnd`. False when Explorer's
// taskbar is not ready (the caller shows tray icons meanwhile and tries again).
bool Create(HINSTANCE inst, HWND notifyWnd, UINT clickMsg, bool lightTheme);
void Destroy();
bool Alive();
void SetItems(const std::vector<WidgetItem>& items, const std::wstring& tooltip);
void SetLightTheme(bool light);
// Draws a rounded plate in the taskbar's colours behind the text (kept across Create/Destroy).
void SetBackground(bool on);
// Re-checks the taskbar layouts, moves the widgets if needed and adds widgets to new taskbars; call
// about once a second. Returns false when a taskbar it was attached to is gone.
bool Reposition();
// Every taskbar window (primary and secondary) with its child windows, for `--taskbars`.
std::wstring DescribeTaskbars();
}
