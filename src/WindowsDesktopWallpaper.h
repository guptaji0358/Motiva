#pragma once

// All raw Win32 desktop-integration logic lives in this module.
// Nothing outside this file/its .cpp should call Win32 desktop APIs
// (Progman / WorkerW / SetParent) directly.

#include <windows.h>
#include <vector>
#include <string>

struct MonitorInfoData {
    HMONITOR handle = nullptr;
    RECT rect{};           // full monitor rect, virtual-desktop (physical px) coords
    RECT workArea{};       // rect minus taskbar
    bool isPrimary = false;
    std::wstring deviceName;
};

// Encapsulates the "attach a window behind the desktop icons" technique:
//   Progman -> WM_052C -> spawns a WorkerW behind SHELLDLL_DefView
//   Our render window is reparented (SetParent) into that WorkerW.
//
// This class owns no video/UI logic. It only knows how to find the right
// host window and reparent/detach arbitrary HWNDs into/out of it.
class WindowsDesktopWallpaper {
public:
    // Enumerates all connected monitors (virtual-desktop/physical coordinates).
    static std::vector<MonitorInfoData> EnumerateMonitors();

    // Locates (or asks Explorer to create) the WorkerW window that sits
    // directly behind the desktop icons, above the classic wallpaper.
    // Returns nullptr if it could not be found/created.
    static HWND FindOrCreateWorkerW();

    // Reparents hwnd into the desktop WorkerW host and positions it to
    // cover the full virtual desktop (all monitors) using physical pixel
    // coordinates. Returns true on success.
    static bool AttachToDesktop(HWND hwnd);

    // Restores hwnd to being a normal top-level window (used on shutdown /
    // "Remove Wallpaper" / error recovery). Does not destroy the window.
    static void DetachFromDesktop(HWND hwnd);

    // Returns true if the previously-found WorkerW handle is still a valid
    // window. Explorer restarts destroy/recreate WorkerW, invalidating any
    // handle we cached — callers should poll this and reattach if false.
    static bool IsWorkerWStillValid(HWND workerW);

    // True only when Explorer has genuinely restarted since the last
    // successful AttachToDesktop() (the host window we attached under is
    // gone, or Progman itself now resolves to a different HWND). This is
    // the ONLY condition that should trigger re-attaching - deliberately
    // does not look at z-order, which changes constantly during normal
    // desktop use (opening windows, Start Menu, etc.) and previously
    // caused a reattach-on-every-perceived-drift loop that flickered.
    // Once attached, the render window should be left alone.
    static bool NeedsReattach();

    // Full virtual desktop bounding rect (union of all monitors), physical px.
    static RECT GetVirtualDesktopRect();

    // Diagnostic-only: logs (qInfo) the full current desktop-shell
    // hierarchy as this module sees it right now - Progman HWND/validity,
    // whichever top-level window currently owns SHELLDLL_DefView, every
    // WorkerW candidate and whether it's plausibly desktop-sized, plus (if
    // wallpaperHwnd is non-null) that window's own GetParent/IsWindow/
    // IsWindowVisible/style state. Always re-derived from live Win32
    // queries - never from this module's own cached g_attached* state -
    // so it answers "what does Explorer's shell actually look like right
    // now" regardless of what we think we last attached to.
    static void DumpDesktopState(HWND wallpaperHwnd = nullptr, const char* context = "");
};
