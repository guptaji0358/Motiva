#include "WindowsDesktopWallpaper.h"
#include <QDebug>
#include <QElapsedTimer>
#include <mutex>

namespace {

// Every entry point below (FindOrCreateWorkerW/AttachToDesktop/
// DetachFromDesktop) reads or writes the module-level g_attached*/
// s_lastFailedDiscovery state further down. WallpaperManager calls
// AttachToDesktop from a background (QtConcurrent) thread, while
// teardownWindows()/DetachFromDesktop can still run on the GUI thread at
// the same time (e.g. the user disables the wallpaper while a reattach
// attempt is in flight) - without a lock that is a data race on plain
// globals, which is undefined behavior and a plausible cause of the
// process dying outright during an Explorer restart rather than just
// failing to attach. Everything in this file that touches that state now
// holds this lock for its whole duration.
std::mutex g_desktopStateMutex;

} // namespace

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

// Set once, on every successful AttachToDesktop() - NOT touched anywhere
// else. NeedsReattach() only ever compares against these; nothing polls or
// re-derives z-order state, which is what previously caused a
// reattach-on-every-perceived-drift loop (visible as flicker).
HWND g_attachedHost = nullptr;
HWND g_attachedProgman = nullptr;
bool g_hasAttached = false;

} // namespace

std::vector<MonitorInfoData> WindowsDesktopWallpaper::EnumerateMonitors() {
    std::vector<MonitorInfoData> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

HWND WindowsDesktopWallpaper::FindOrCreateWorkerW() {
    QElapsedTimer stageTimer;
    stageTimer.start();
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        qWarning() << "[Discover] Progman window not found - "
                       "no Explorer desktop shell is active in this session yet.";
        return nullptr;
    }
    qInfo() << "[Discover] Progman found, hwnd=" << reinterpret_cast<quintptr>(progman);

    HWND iconOwnerCheap = nullptr;
    HWND cheapWorker = FindWorkerWBehindIcons(&iconOwnerCheap);
    qInfo() << "[Discover] Cheap WorkerW/SHELLDLL_DefView probe took" << stageTimer.elapsed()
            << "ms, iconOwner=" << reinterpret_cast<quintptr>(iconOwnerCheap)
            << "worker=" << reinterpret_cast<quintptr>(cheapWorker);
    if (cheapWorker) {
        return cheapWorker;
    }
    if (!iconOwnerCheap) {
        // SHELLDLL_DefView (the desktop icon layer) does not exist under
        // ANY top-level window yet. This is the state observed right after
        // boot, before Explorer has finished building the desktop - the
        // 0x052C spawn-message dance below is pointless until this exists,
        // since it only asks Explorer to create a WorkerW *alongside* an
        // icon layer that isn't there yet. Bail out immediately (no
        // Sleep-based busy-wait) and let the caller's short-interval retry
        // timer check again shortly, instead of burning several seconds
        // here on a doomed discovery sequence.
        qInfo() << "[Discover] No SHELLDLL_DefView anywhere yet - desktop icon layer "
                    "not built by Explorer yet. Skipping spawn-message probe this round.";
        return nullptr;
    }

    // The full discovery below sends spawn messages and busy-waits for
    // Explorer to react (up to several seconds total). WallpaperManager's
    // health check calls this on every reattach, and on a build where no
    // real WorkerW is ever created (as confirmed by testing on this
    // machine) that full sequence would otherwise re-run - and re-block
    // the GUI thread for seconds at a time - every single health-check
    // tick, which is what caused the app to become genuinely unresponsive
    // ("Not Responding") rather than just occasionally re-pinning a
    // window. Remember a negative result for a cooldown window so repeat
    // callers get the cheap "definitely not available right now" answer
    // instead of redoing the whole expensive dance.
    static ULONGLONG s_lastFailedDiscovery = 0;
    // 5 minutes: long enough that, on a machine where discovery
    // deterministically never succeeds (confirmed by testing - this
    // Explorer build never creates a real WorkerW), the GUI thread isn't
    // repeatedly stalled for several seconds by a doomed retry; short
    // enough to notice a real WorkerW appearing after an Explorer
    // restart within a reasonable time.
    constexpr ULONGLONG kRediscoveryCooldownMs = 5 * 60 * 1000;
    const ULONGLONG now = GetTickCount64();
    if (s_lastFailedDiscovery != 0 && (now - s_lastFailedDiscovery) < kRediscoveryCooldownMs) {
        // Known-failed recently: skip straight to the same Progman
        // fallback a full failed discovery would have ended in, without
        // re-running the expensive probe. Returning nullptr here would be
        // wrong - AttachToDesktop treats that as "no host available at
        // all" and hides the window instead of falling back.
        return progman;
    }

    // Ask Explorer to spawn the WorkerW layer. Undocumented but stable
    // since Windows 7 and still functions on Windows 10/11 as of this
    // writing. Different Explorer builds have been observed to only react
    // to one or another of these known wParam/lParam variants.
    //
    // IMPORTANT: on at least one real Windows 11 build this message was
    // found (by testing) to behave as a *toggle* rather than an idempotent
    // "ensure it exists" - sending the same variant twice in a row created
    // the WorkerW and then immediately destroyed it again, so a batch of
    // four sends with no check in between could easily net out to "no
    // WorkerW" even though Explorer is perfectly capable of creating one.
    // Send one variant at a time and check for a real result after each
    // individual send, stopping the instant one appears.
    struct Variant { WPARAM wParam; LPARAM lParam; };
    const Variant variants[] = { {0xD, 0x1}, {0, 0}, {0, 1} };

    HWND iconOwner = iconOwnerCheap;
    HWND worker = nullptr;

    for (const auto& v : variants) {
        if (worker) {
            break;
        }
        DWORD_PTR result = 0;
        SendMessageTimeoutW(progman, 0x052C, v.wParam, v.lParam, SMTO_NORMAL, 1000, &result);
        for (int i = 0; i < 20 && !worker; ++i) {
            Sleep(50);
            worker = FindWorkerWBehindIcons(&iconOwner);
        }
    }

    if (worker) {
        return worker;
    }

    // Still nothing: re-applying the current desktop wallpaper forces
    // Explorer to rebuild its whole desktop-rendering pipeline from
    // scratch, which on some builds is what it takes to get it to create a
    // proper, DWM-recognized WorkerW. Try the same one-at-a-time sequence
    // again afterwards.
    qWarning() << "[WindowsDesktopWallpaper] No WorkerW after spawn messages; "
                   "re-applying wallpaper to force Explorer to rebuild the "
                   "desktop and retrying.";
    wchar_t currentWallpaper[MAX_PATH] = {};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, currentWallpaper, 0)) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, currentWallpaper,
                               SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }
    Sleep(300);
    for (const auto& v : variants) {
        if (worker) {
            break;
        }
        DWORD_PTR result = 0;
        SendMessageTimeoutW(progman, 0x052C, v.wParam, v.lParam, SMTO_NORMAL, 1000, &result);
        for (int i = 0; i < 20 && !worker; ++i) {
            Sleep(50);
            worker = FindWorkerWBehindIcons(&iconOwner);
        }
    }

    if (worker) {
        return worker;
    }

    // Definitively failed this round - remember it so the next call
    // (likely from the 3s health-check timer) skips straight past the
    // expensive rediscovery until the cooldown expires, rather than
    // repeating a multi-second, GUI-thread-blocking probe every tick.
    s_lastFailedDiscovery = now;

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
    // See the g_desktopStateMutex comment above: this can run on a
    // background thread (WallpaperManager's async attach) concurrently
    // with DetachFromDesktop on the GUI thread. Held for the whole
    // discovery+reparent sequence, not just the state writes at the end,
    // so a concurrent Detach can't yank g_attachedHost/g_hasAttached out
    // from under a reparent that's still in progress.
    std::lock_guard<std::mutex> lock(g_desktopStateMutex);

    QElapsedTimer attachTimer;
    attachTimer.start();

    HWND worker = FindOrCreateWorkerW();
    qInfo() << "[Discover] FindOrCreateWorkerW took" << attachTimer.elapsed()
            << "ms, result=" << reinterpret_cast<quintptr>(worker);
    if (!worker) {
        qWarning() << "[WindowsDesktopWallpaper] AttachToDesktop: no WorkerW/Progman host available yet.";
        return false;
    }

    HWND progman = FindWindowW(L"Progman", nullptr);

    // Make sure the window isn't flagged topmost - SetParent into a
    // non-topmost host reliably fails (ERROR_INVALID_PARAMETER) while a
    // window still carries topmost z-order state, and clearing the
    // WS_EX_TOPMOST bit alone via SetWindowLongPtr does not undo that
    // z-order state; it has to be cleared via SetWindowPos.
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    // Overwrite (not mask) the style/exstyle to a clean, known-valid
    // combination for a reparented child window.
    //
    // WS_EX_NOREDIRECTIONBITMAP is load-bearing here, not cosmetic: live
    // diagnostics on this machine (see CLAUDE.md) found that Progman
    // itself carries this exact extended style. Windows normally gives a
    // window its own GDI/DWM redirection bitmap for classic WM_PAINT
    // content; WS_EX_NOREDIRECTIONBITMAP opts a window out of that in
    // favor of presenting through DirectComposition instead. A plain GDI
    // child we previously attached behind SHELLDLL_DefView was verified,
    // via this same diagnostic tooling, to be geometrically correct and
    // IsWindowVisible==true yet still not visually render - consistent
    // with Progman's own redirection-bypassed compositing not picking up
    // an ordinary GDI-painted child's content. Setting this style here,
    // and presenting via a DirectComposition visual bound to this HWND
    // (see D3DWallpaperRenderer) instead of WM_PAINT/GDI, matches the
    // same compositing substrate Progman itself uses.
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_CHILD | WS_VISIBLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, WS_EX_NOREDIRECTIONBITMAP);

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
    g_attachedHost = worker;
    g_attachedProgman = progman;
    g_hasAttached = true;
    qInfo() << "[Wallpaper] AttachToDesktop succeeded for hwnd=" << reinterpret_cast<quintptr>(hwnd)
            << "total time" << attachTimer.elapsed() << "ms.";
    return true;
}

void WindowsDesktopWallpaper::DetachFromDesktop(HWND hwnd) {
    std::lock_guard<std::mutex> lock(g_desktopStateMutex);
    if (!hwnd || !IsWindow(hwnd)) {
        return;
    }

    ShowWindow(hwnd, SW_HIDE);
    SetParent(hwnd, nullptr);

    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style &= ~WS_CHILD;
    style |= WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    g_hasAttached = false;
    g_attachedHost = nullptr;
    g_attachedProgman = nullptr;
}

bool WindowsDesktopWallpaper::IsWorkerWStillValid(HWND workerW) {
    return workerW != nullptr && IsWindow(workerW);
}

bool WindowsDesktopWallpaper::NeedsReattach() {
    if (!g_hasAttached) {
        return false;
    }
    HWND currentProgman = FindWindowW(L"Progman", nullptr);
    if (!currentProgman || currentProgman != g_attachedProgman) {
        return true; // Explorer restarted - Progman itself is a new HWND
    }
    if (!IsWindow(g_attachedHost)) {
        return true; // our host window was destroyed
    }
    return false;
}

void WindowsDesktopWallpaper::DumpDesktopState(HWND wallpaperHwnd, const char* context) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    DWORD progmanPid = 0;
    if (progman) {
        GetWindowThreadProcessId(progman, &progmanPid);
    }
    HWND iconOwner = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            if (FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr)) {
                *reinterpret_cast<HWND*>(lParam) = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&iconOwner));
    HWND shellDefView = iconOwner ? FindWindowExW(iconOwner, nullptr, L"SHELLDLL_DefView", nullptr) : nullptr;

    qInfo() << "[Diag]" << context << "Explorer PID(via Progman)=" << progmanPid
            << "Progman=" << reinterpret_cast<quintptr>(progman)
            << "valid=" << (progman && IsWindow(progman))
            << "| SHELLDLL_DefView owner=" << reinterpret_cast<quintptr>(iconOwner)
            << "SHELLDLL_DefView=" << reinterpret_cast<quintptr>(shellDefView);

    HWND w = nullptr;
    int i = 0;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        RECT r{};
        GetWindowRect(w, &r);
        bool hasShellView = FindWindowExW(w, nullptr, L"SHELLDLL_DefView", nullptr) != nullptr;
        qInfo() << "[Diag]" << context << "WorkerW[" << i << "] hwnd=" << reinterpret_cast<quintptr>(w)
                << "visible=" << (bool)IsWindowVisible(w) << "size=" << (r.right - r.left) << "x"
                << (r.bottom - r.top) << "hasShellView=" << hasShellView;
        ++i;
    }
    if (i == 0) {
        qInfo() << "[Diag]" << context << "No top-level WorkerW windows exist right now.";
    }

    if (wallpaperHwnd) {
        const bool isValid = IsWindow(wallpaperHwnd);
        HWND parent = isValid ? GetParent(wallpaperHwnd) : nullptr;
        LONG_PTR style = isValid ? GetWindowLongPtrW(wallpaperHwnd, GWL_STYLE) : 0;
        LONG_PTR exStyle = isValid ? GetWindowLongPtrW(wallpaperHwnd, GWL_EXSTYLE) : 0;
        RECT r{};
        if (isValid) {
            GetWindowRect(wallpaperHwnd, &r);
        }
        qInfo() << "[Diag]" << context << "wallpaperHwnd=" << reinterpret_cast<quintptr>(wallpaperHwnd)
                << "IsWindow=" << isValid << "GetParent=" << reinterpret_cast<quintptr>(parent)
                << "parentIsProgman=" << (parent == progman) << "parentIsWorkerW=" << (parent != progman && parent != nullptr)
                << "IsWindowVisible=" << (bool)IsWindowVisible(wallpaperHwnd)
                << "style=0x" << Qt::hex << (unsigned long)style << "exStyle=0x" << (unsigned long)exStyle << Qt::dec
                << "rect=" << r.left << r.top << r.right << r.bottom;
    }
}

RECT WindowsDesktopWallpaper::GetVirtualDesktopRect() {
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}
