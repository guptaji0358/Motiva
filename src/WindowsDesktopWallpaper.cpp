#include "WindowsDesktopWallpaper.h"
#include <QDebug>

namespace {

BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam) {
    auto* out = reinterpret_cast<std::vector<MonitorInfoData>*>(lParam);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(hMon, &info)) {
        return TRUE;
    }

    MonitorInfoData data;
    data.handle = hMon;
    data.rect = info.rcMonitor;
    data.workArea = info.rcWork;
    data.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    data.deviceName = info.szDevice;
    out->push_back(data);
    return TRUE;
}

// The WorkerW we want is the one that:
//  - is a top-level "WorkerW" window
//  - has NO "SHELLDLL_DefView" child (that one belongs to the icon layer)
//  - sits immediately behind (i.e. is the next Z-order sibling after) the
//    window that DOES own SHELLDLL_DefView
//
// Strategy (the canonical, widely-reproduced technique on Win10/11):
//   1. Send 0x052C to Progman. This undocumented message asks Explorer to
//      spawn a WorkerW as a sibling of Progman, hosting the desktop icons
//      (SHELLDLL_DefView), if one does not already exist. Some Explorer
//      builds need the message sent more than once and/or a moment to react.
//   2. Use EnumWindows (all top-level windows, in Z-order) to find whichever
//      top-level window currently owns a SHELLDLL_DefView child - this is
//      normally Progman itself, but on some builds it is a WorkerW.
//   3. FindWindowExW(NULL, iconOwner, L"WorkerW", NULL) then finds the next
//      WorkerW sibling *after* the icon owner in Z-order - the icon-less
//      WorkerW Explorer already paints directly beneath the icons. That is
//      the window we attach our own render window to.
HWND FindWorkerWBehindIcons(HWND* outIconOwner = nullptr) {
    HWND iconOwner = nullptr;

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *reinterpret_cast<HWND*>(lParam) = hwnd;
                return FALSE; // stop enumeration, found it
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&iconOwner));

    if (outIconOwner) {
        *outIconOwner = iconOwner;
    }

    if (!iconOwner) {
        return nullptr;
    }

    // A real wallpaper-hosting WorkerW spans the desktop; Windows also
    // creates numerous unrelated tiny/invisible windows that happen to
    // reuse the "WorkerW" window class (observed: 166x47, always
    // invisible) for things unrelated to the desktop. Reject those so we
    // never attach into one by accident - a real target should cover a
    // meaningful area (at least roughly monitor-sized).
    constexpr LONG kMinDimension = 200;
    auto isPlausibleDesktopSized = [](HWND w) {
        RECT r{};
        if (!GetWindowRect(w, &r)) {
            return false;
        }
        return (r.right - r.left) >= kMinDimension && (r.bottom - r.top) >= kMinDimension;
    };

    // Next WorkerW sibling after the icon owner, walking top-level Z-order.
    HWND candidate = FindWindowExW(nullptr, iconOwner, L"WorkerW", nullptr);
    if (candidate && isPlausibleDesktopSized(candidate) &&
        !FindWindowExW(candidate, nullptr, L"SHELLDLL_DefView", nullptr)) {
        return candidate;
    }

    // Some builds place the plain WorkerW *before* the icon owner instead of
    // after. Fall back to scanning all WorkerW windows for one with no
    // SHELLDLL_DefView child that is plausibly desktop-sized.
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        if (w != iconOwner && isPlausibleDesktopSized(w) &&
            !FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr)) {
            return w;
        }
    }

    // No plausible standalone WorkerW exists at all (observed on some
    // Windows 11 builds/configurations: Explorer never spawns one via the
    // 0x052C message, or removes it once no separate icon-less WorkerW is
    // needed). The icon owner itself (typically Progman, correctly sized
    // to the desktop) is then the best available attach target.
    return nullptr;
}

void DumpAllWorkerWWindows() {
    HWND w = nullptr;
    int i = 0;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        RECT r{};
        GetWindowRect(w, &r);
        bool hasShellView = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
        qWarning() << "[WindowsDesktopWallpaper] WorkerW[" << i << "] hwnd="
                    << reinterpret_cast<quintptr>(w)
                    << "visible=" << (bool)IsWindowVisible(w)
                    << "rect=" << r.left << r.top << r.right << r.bottom
                    << "hasShellView=" << hasShellView;
        ++i;
    }
    if (i == 0) {
        qWarning() << "[WindowsDesktopWallpaper] No top-level WorkerW windows exist at all.";
    }
}

} // namespace

std::vector<MonitorInfoData> WindowsDesktopWallpaper::EnumerateMonitors() {
    std::vector<MonitorInfoData> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

HWND WindowsDesktopWallpaper::FindOrCreateWorkerW() {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        qWarning() << "[WindowsDesktopWallpaper] Progman window not found - "
                       "no Explorer desktop shell is active in this session.";
        return nullptr;
    }

    // Ask Explorer to spawn the WorkerW layer. Undocumented but stable
    // since Windows 7 and still functions on Windows 10/11 as of this
    // writing. Different Explorer builds have been observed to only react
    // to one or the other of these two known wParam/lParam variants, so we
    // try both. SendMessageTimeout avoids hanging if Explorer is busy.
    auto sendSpawnMessages = [&] {
        DWORD_PTR result = 0;
        SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
        SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &result);
        SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &result);
        SendMessageTimeoutW(progman, 0x052C, 0, 1, SMTO_NORMAL, 1000, &result);
    };
    sendSpawnMessages();

    HWND iconOwner = nullptr;
    HWND worker = FindWorkerWBehindIcons(&iconOwner);

    // Some Explorer versions need a brief moment to create the window
    // after the message is processed. Retry for up to ~1s.
    for (int i = 0; i < 20 && !worker; ++i) {
        Sleep(50);
        worker = FindWorkerWBehindIcons(&iconOwner);
    }

    if (worker) {
        return worker;
    }

    // Some Explorer builds simply never spawn a WorkerW via the 0x052C
    // message alone. Re-applying the current desktop wallpaper forces
    // Explorer to rebuild its whole desktop-rendering pipeline from
    // scratch (this is the fix real-world video-wallpaper tools use for
    // this exact case) - after that rebuild it reliably does create a
    // proper, DWM-recognized WorkerW. Only worth trying once; if it
    // doesn't help there is nothing more to nudge.
    qWarning() << "[WindowsDesktopWallpaper] No WorkerW after spawn message; "
                   "re-applying wallpaper to force Explorer to rebuild the "
                   "desktop and retrying.";
    wchar_t currentWallpaper[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, currentWallpaper, 0)) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, currentWallpaper,
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    Sleep(300);
    sendSpawnMessages();
    worker = FindWorkerWBehindIcons(&iconOwner);
    for (int i = 0; i < 20 && !worker; ++i) {
        Sleep(50);
        worker = FindWorkerWBehindIcons(&iconOwner);
    }

    if (worker) {
        return worker;
    }

    if (!iconOwner) {
        qWarning() << "[WindowsDesktopWallpaper] No window with a "
                       "SHELLDLL_DefView child was found (desktop icon "
                       "layer not located) - cannot determine where to "
                       "attach the wallpaper window.";
        return nullptr;
    }

    // Last-resort fallback: some minimal/locked-down desktop configurations
    // never create a separate plain WorkerW at all. Attaching directly to
    // Progman still places the render window behind the icon-owning
    // window's normal paint order in most such cases, which is far better
    // than not attaching at all.
    qWarning() << "[WindowsDesktopWallpaper] Icon-less WorkerW sibling not "
                   "found; falling back to attaching directly to Progman.";
    return progman;
}

bool WindowsDesktopWallpaper::AttachToDesktop(HWND hwnd) {
    if (!hwnd) {
        return false;
    }

    HWND worker = FindOrCreateWorkerW();
    if (!worker) {
        qWarning() << "[WindowsDesktopWallpaper] AttachToDesktop: no WorkerW/Progman host available.";
        return false;
    }

    HWND progman = FindWindowW(L"Progman", nullptr);
    const bool isProgmanFallback = (worker == progman);

    if (isProgmanFallback) {
        // Some Explorer builds never spawn the icon-less WorkerW that the
        // classic technique relies on (confirmed by testing: only small,
        // unrelated 166x47 WorkerW windows exist here, none desktop-sized).
        // DWM only composites content between the wallpaper backdrop and
        // the icon layer for that specific, Explorer-created WorkerW - a
        // window we reparent as a *child* of Progman itself does not get
        // that special treatment and stays invisible even though it is
        // correctly sized, positioned, and reports IsWindowVisible==true
        // (confirmed by testing). The fix is to not reparent into Progman
        // at all: keep our window top-level and place it immediately
        // behind Progman in the *global* top-level z-order instead, which
        // is genuinely above the desktop backdrop (a top-level window is
        // by definition rendered above it) while still sitting below
        // Progman's own icon view.
        SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
        SetParent(hwnd, nullptr);

        RECT vd = GetVirtualDesktopRect();
        SetWindowPos(hwnd, progman, vd.left, vd.top,
                     vd.right - vd.left, vd.bottom - vd.top,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        return true;
    }

    // Make sure the window isn't flagged topmost - SetParent into a
    // non-topmost host reliably fails (ERROR_INVALID_PARAMETER) while a
    // window still carries topmost z-order state, and clearing the
    // WS_EX_TOPMOST bit alone via SetWindowLongPtr does not undo that
    // z-order state; it has to be cleared via SetWindowPos.
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Overwrite (not mask) the style/exstyle: Qt's own window flags for a
    // frameless Qt::Tool widget can leave bit combinations (e.g. WS_POPUP
    // together with layered/toolwindow ex-styles) that are invalid once
    // combined with WS_CHILD, which is what actually made SetParent below
    // fail with ERROR_INVALID_PARAMETER. A plain child window only needs
    // WS_CHILD | WS_VISIBLE and no extended styles.
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, 0);

    // SetParent between windows with mismatched thread DPI-awareness
    // contexts (our Per-Monitor-V2-aware window vs. Explorer's
    // System-DPI-aware WorkerW/Progman) reliably fails with
    // ERROR_INVALID_PARAMETER on Windows 10/11. Temporarily switch this
    // thread to System-aware for the duration of the call only; the
    // window's own per-monitor pixel geometry is still applied afterwards
    // via SetWindowPos in physical pixels, so this doesn't affect layout.
    DPI_AWARENESS_CONTEXT prevDpiContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    SetLastError(0);
    HWND prevParent = SetParent(hwnd, worker);
    DWORD setParentErr = GetLastError();

    if (prevDpiContext) {
        SetThreadDpiAwarenessContext(prevDpiContext);
    }

    if (prevParent == nullptr) {
        qWarning() << "[WindowsDesktopWallpaper] SetParent failed, GetLastError=" << setParentErr
                    << "worker=" << reinterpret_cast<quintptr>(worker)
                    << "hwnd=" << reinterpret_cast<quintptr>(hwnd);
        DumpAllWorkerWWindows();
        return false;
    }

    RECT vd = GetVirtualDesktopRect();

    // Where exactly to place hwnd in the z-order matters a lot here. If
    // `worker` is a genuine icon-less WorkerW, nothing else lives in it, so
    // the bottom of its (empty) child z-order is fine. But when we fell
    // back to attaching directly into Progman - which also parents the
    // desktop icon view (SHELLDLL_DefView) AND, on this Windows build,
    // whatever else actually paints the classic wallpaper backdrop - the
    // absolute bottom of Progman's children turned out to sit BELOW that
    // backdrop paint too, making the window attach successfully yet render
    // completely invisibly (confirmed by testing: HWND_BOTTOM = invisible,
    // top of z-order = visible but covering the icons). The fix is to
    // target the exact z-slot immediately behind the icon view instead of
    // the blunt "bottom of everything": SetWindowPos's hwndInsertAfter set
    // to the icon view's own handle places hwnd directly behind it,
    // pushing whatever used to occupy that slot (the backdrop) further
    // down - above the backdrop, below the icons, exactly where a real
    // icon-less WorkerW would normally sit.
    HWND shellDefView = FindWindowExW(worker, nullptr, L"SHELLDLL_DefView", nullptr);
    HWND zOrderTarget = shellDefView ? shellDefView : HWND_BOTTOM;

    SetWindowPos(hwnd, zOrderTarget,
                 vd.left, vd.top,
                 vd.right - vd.left, vd.bottom - vd.top,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void WindowsDesktopWallpaper::DetachFromDesktop(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    ShowWindow(hwnd, SW_HIDE);
    SetParent(hwnd, nullptr);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~WS_CHILD;
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);
}

bool WindowsDesktopWallpaper::IsWorkerWStillValid(HWND workerW) {
    return workerW != nullptr && IsWindow(workerW);
}

RECT WindowsDesktopWallpaper::GetVirtualDesktopRect() {
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}
