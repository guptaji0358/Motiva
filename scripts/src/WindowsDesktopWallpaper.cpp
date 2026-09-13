#include "WindowsDesktopWallpaper.h"
#include "StartupDiagnostics.h"
#include <QDebug>
#include <QElapsedTimer>
#include <mutex>
#include <dwmapi.h>
#include <shobjidl.h>

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

// One line of full detail for a single HWND - class, title, PID/TID,
// parent, owner, visible/enabled, style/exstyle, window+client rect, and
// DWM cloaked state (a window can be IsWindowVisible==true and still be
// invisible to the user if DWM has cloaked it - DWMWA_CLOAKED is the only
// way to detect that; IsWindowVisible alone reports the Win32-level
// visibility flag regardless of DWM composition state).
void LogWindowDetail(const char* context, const char* label, HWND hwnd) {
    if (!hwnd) {
        return;
    }
    wchar_t className[256] = {};
    GetClassNameW(hwnd, className, 256);
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    HWND parent = GetParent(hwnd);
    HWND owner = GetWindow(hwnd, GW_OWNER);
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    RECT wr{};
    GetWindowRect(hwnd, &wr);
    RECT cr{};
    GetClientRect(hwnd, &cr);
    DWORD cloaked = 0;
    HRESULT cloakedHr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));

    qInfo() << "[Hierarchy]" << context << label << "hwnd=" << reinterpret_cast<quintptr>(hwnd)
            << "class=" << QString::fromWCharArray(className)
            << "title=" << QString::fromWCharArray(title)
            << "pid=" << pid << "tid=" << tid
            << "parent=" << reinterpret_cast<quintptr>(parent)
            << "owner=" << reinterpret_cast<quintptr>(owner)
            << "visible=" << (bool)IsWindowVisible(hwnd) << "enabled=" << (bool)IsWindowEnabled(hwnd)
            << "cloaked=" << (SUCCEEDED(cloakedHr) ? QString::number(cloaked) : QStringLiteral("n/a"))
            << "style=0x" << Qt::hex << (unsigned long)style << "exStyle=0x" << (unsigned long)exStyle << Qt::dec
            << "windowRect=" << wr.left << wr.top << wr.right << wr.bottom
            << "clientRect=" << cr.left << cr.top << cr.right << cr.bottom;
}

// Logs every child of `parent`, in z-order (EnumChildWindows visits
// top-level-first-then-recurses, but for a single level it walks in
// genuine top-to-bottom z-order), tagging our own process's windows and
// SHELLDLL_DefView/SysListView32/FolderView specifically since those are
// the ones item 5 of the diagnostic ask cares about.
void LogChildren(const char* context, const char* parentLabel, HWND parent) {
    if (!parent) {
        return;
    }
    struct Ctx { const char* context; const char* parentLabel; int index; };
    Ctx ctx{context, parentLabel, 0};
    EnumChildWindows(
        parent,
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lParam);
            wchar_t className[256] = {};
            GetClassNameW(hwnd, className, 256);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            const bool isOwnProcess = (pid == GetCurrentProcessId());
            QString label = QString("child[%1] of %2%3").arg(c->index).arg(c->parentLabel)
                                 .arg(isOwnProcess ? " OURS" : "");
            LogWindowDetail(c->context, label.toUtf8().constData(), hwnd);
            ++c->index;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}

// Set once, on every successful AttachToDesktop() - NOT touched anywhere
// else. NeedsReattach() only ever compares against these; nothing polls or
// re-derives z-order state, which is what previously caused a
// reattach-on-every-perceived-drift loop (visible as flicker).
HWND g_attachedHost = nullptr;
HWND g_attachedProgman = nullptr;
bool g_hasAttached = false;

// Guards the bounded one-shot 0x052C spawn message in FindOrCreateWorkerW
// so it fires at most once per attach generation, not on every call.
// WallpaperManager's 300ms retry timer re-enters FindOrCreateWorkerW on
// every tick for as long as attach hasn't been verified (e.g. indefinitely
// on a machine where no standalone WorkerW is ever created - see
// CLAUDE.md), and without this guard every one of those ticks resent the
// spawn message to Explorer's Progman, asking it to recreate/rehost the
// desktop icon layer's WorkerW up to ~3x/second, unbounded, for as long as
// our wallpaper mode stayed active - a continuous, unthrottled mutation of
// Explorer's OWN desktop shell hierarchy. That is the confirmed root cause
// of Explorer's unrelated "Set as desktop background" occasionally
// breaking while this app was running - see CLAUDE.md's 2026-09-11 "Set as
// wallpaper interference" entry. A fresh generation (Explorer restart, or
// a new attach cycle after teardown) always gets its own one-shot attempt,
// exactly matching FindOrCreateWorkerW's own header documentation for this
// parameter, which promised generation-scoped throttling that had
// regressed to Q_UNUSED.
quint64 s_lastSpawnAttemptGeneration = 0;
bool s_hasAttemptedSpawnForGeneration = false;

} // namespace

std::vector<MonitorInfoData> WindowsDesktopWallpaper::EnumerateMonitors() {
    std::vector<MonitorInfoData> monitors;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

HWND WindowsDesktopWallpaper::FindOrCreateWorkerW(uint64_t generation) {
    QElapsedTimer stageTimer;
    stageTimer.start();
    StartupDiagnostics::instance().mark("desktopDiscoveryStart");
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (!progman) {
        qWarning() << "[Discover] Progman window not found - "
                       "no Explorer desktop shell is active in this session yet.";
        return nullptr;
    }
    qInfo() << "[Discover] Progman found, hwnd=" << reinterpret_cast<quintptr>(progman);
    StartupDiagnostics::instance().mark("explorerDetected");

    HWND iconOwnerCheap = nullptr;
    HWND cheapWorker = FindWorkerWBehindIcons(&iconOwnerCheap);
    if (iconOwnerCheap) {
        StartupDiagnostics::instance().mark("desktopHierarchyDetected");
    }
    qInfo() << "[Discover] Cheap WorkerW/SHELLDLL_DefView probe took" << stageTimer.elapsed()
            << "ms, iconOwner=" << reinterpret_cast<quintptr>(iconOwnerCheap)
            << "worker=" << reinterpret_cast<quintptr>(cheapWorker);
    if (cheapWorker) {
        StartupDiagnostics::instance().mark("requiredShellWindowFound");
        return cheapWorker;
    }
    if (!iconOwnerCheap) {
        // SHELLDLL_DefView (the desktop icon layer) does not exist under
        // ANY top-level window yet. This is the state observed right after
        // boot, before Explorer has finished building the desktop. Bail
        // out immediately (no Sleep-based busy-wait) and let the caller's
        // short-interval retry timer check again shortly, instead of
        // burning time here on a doomed discovery attempt.
        qInfo() << "[Discover] No SHELLDLL_DefView anywhere yet - desktop icon layer "
                    "not built by Explorer yet. Skipping spawn-message probe this round.";
        return nullptr;
    }

    // No standalone icon-less WorkerW exists yet. The 2026-09-09 fix
    // removed the old multi-second discovery dance here (6 message sends
    // across 2 rounds, each followed by up to 1s of Sleep-based polling,
    // plus a forced wallpaper-rebuild pause) in favor of attaching directly
    // to Progman immediately. That eliminated the startup delay, but a
    // follow-up investigation (see CLAUDE.md "wallpaper visibility
    // investigation") found via full desktop-hierarchy diagnostics that
    // the Progman-direct fallback - though correctly parented, positioned,
    // sized, and uncloaked by every Win32/DWM signal available - does not
    // actually get composited above the OS-painted desktop wallpaper on
    // this machine; only a genuine icon-less WorkerW does. That was never
    // actually visually verified as working in any prior session (only
    // inferred from AttachToDesktop()==true and presentedFrameCount
    // advancing, neither of which proves real screen visibility).
    //
    // Per explicit user approval, a single BOUNDED spawn attempt is
    // allowed here - deliberately NOT the old multi-variant/multi-round
    // polling dance: exactly one message send (the historically most
    // effective variant) with a bounded SendMessageTimeoutW timeout, one
    // short fixed settle wait (not a retry loop), and one check. Worst
    // case this adds ~150ms when no real WorkerW exists, not seconds, and
    // never blocks the GUI thread (this whole function already only runs
    // on WallpaperManager's background attach thread). If it doesn't
    // produce a real WorkerW, we fall straight back to the Progman
    // fallback exactly as before - this is strictly additive, not a
    // dependency the fallback needs.
    //
    // "Bounded" and "one-shot" only hold if this is actually throttled:
    // this function is re-entered on every one of WallpaperManager's
    // 300ms retry-timer ticks while attach keeps failing/isn't yet
    // verified, so without a per-generation guard the spawn message below
    // was actually being resent to Explorer roughly 3x/second, unbounded,
    // for as long as that retry loop ran - see s_hasAttemptedSpawnForGeneration's
    // comment for why that is the confirmed root cause of Explorer's own
    // "Set as desktop background" occasionally breaking while this app is
    // running. Skip straight to the Progman fallback if this generation
    // already had its one attempt.
    if (s_hasAttemptedSpawnForGeneration && s_lastSpawnAttemptGeneration == generation) {
        qInfo() << "[Discover] Already made this attach generation's one-shot spawn attempt - "
                    "not resending 0x052C to Explorer again; falling back to Progman hwnd="
                << reinterpret_cast<quintptr>(progman) << ".";
        StartupDiagnostics::instance().mark("requiredShellWindowFound");
        return progman;
    }
    s_hasAttemptedSpawnForGeneration = true;
    s_lastSpawnAttemptGeneration = generation;

    DWORD_PTR spawnResult = 0;
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 200, &spawnResult);
    Sleep(100); // single bounded settle wait, not a retry loop - see comment above
    HWND iconOwnerAfterSpawn = iconOwnerCheap;
    HWND spawnedWorker = FindWorkerWBehindIcons(&iconOwnerAfterSpawn);
    if (spawnedWorker) {
        qInfo() << "[Discover] Bounded one-shot spawn attempt succeeded - real WorkerW hwnd="
                << reinterpret_cast<quintptr>(spawnedWorker);
        StartupDiagnostics::instance().mark("requiredShellWindowFound");
        return spawnedWorker;
    }

    StartupDiagnostics::instance().mark("requiredShellWindowFound");
    qInfo() << "[Discover] No standalone WorkerW after one bounded spawn attempt - "
                "attaching directly behind Progman instead hwnd=" << reinterpret_cast<quintptr>(progman)
            << "(see CLAUDE.md - this fallback's actual visibility on this machine is under "
                "investigation; this call site is unchanged either way).";
    return progman;
}

bool WindowsDesktopWallpaper::AttachToDesktop(HWND hwnd, uint64_t generation) {
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

    StartupDiagnostics::instance().mark("attachToDesktopStart");
    QElapsedTimer attachTimer;
    attachTimer.start();

    HWND worker = FindOrCreateWorkerW(generation);
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
    StartupDiagnostics::instance().mark("wallpaperHostCreated");
    StartupDiagnostics::instance().mark("wallpaperAttached");
    StartupDiagnostics::instance().mark("attachToDesktopEnd");
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

std::wstring WindowsDesktopWallpaper::GetCurrentWallpaperPath() {
    // Prefer the modern per-monitor wallpaper API (IDesktopWallpaper,
    // available since Windows 8) over the legacy single-string
    // SPI_GETDESKWALLPAPER below. Windows 11's own Settings app
    // (Personalization > Background) and, on multi-monitor setups,
    // Explorer's own "Set as desktop background" go through this COM API
    // rather than the legacy SPI call - SPI_GETDESKWALLPAPER was found to
    // not always reflect a wallpaper picked that way (stale or empty),
    // which would have silently broken the external-change detection this
    // function feeds (see WallpaperManager::onPossibleExternalWallpaperChange -
    // "our video keeps covering the desktop after picking a new Windows
    // wallpaper" traces back to exactly this: the baseline/current
    // comparison never saw a difference because this function was reading
    // the wrong source). Falls through to the legacy SPI call if COM is
    // unavailable for any reason, which keeps this working exactly as
    // before on any path where the modern API doesn't apply.
    std::wstring comResult;
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool weInitializedCom = (coInit == S_OK);
    if (SUCCEEDED(coInit) || coInit == RPC_E_CHANGED_MODE) {
        IDesktopWallpaper* wallpaper = nullptr;
        // CLSID_DesktopWallpaper is hosted by an out-of-process COM
        // surrogate ("Desktop Wallpaper Factory", confirmed via its
        // registered AppID - RunAs=Interactive User) rather than an
        // in-proc DLL: CLSCTX_INPROC_SERVER alone fails with
        // REGDB_E_CLASSNOTREG (confirmed by testing) since no
        // InprocServer32 key exists for this CLSID at all, only AppID +
        // (implicitly) LocalServer32 activation. CLSCTX_LOCAL_SERVER is
        // required.
        if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_LOCAL_SERVER,
                                        IID_PPV_ARGS(&wallpaper))) && wallpaper) {
            // Monitor 0 is enough here - this function's only job is
            // detecting "the user changed their wallpaper at all", not
            // managing per-monitor images (Motiva doesn't do that).
            LPWSTR monitorId = nullptr;
            if (SUCCEEDED(wallpaper->GetMonitorDevicePathAt(0, &monitorId)) && monitorId) {
                LPWSTR path = nullptr;
                if (SUCCEEDED(wallpaper->GetWallpaper(monitorId, &path)) && path) {
                    comResult = path;
                    CoTaskMemFree(path);
                }
                CoTaskMemFree(monitorId);
            }
            wallpaper->Release();
        }
    }
    if (weInitializedCom) {
        CoUninitialize();
    }
    if (!comResult.empty()) {
        return comResult;
    }

    // Legacy fallback - what this function exclusively did before.
    wchar_t currentWallpaper[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, currentWallpaper, 0)) {
        qWarning() << "[Wallpaper] GetCurrentWallpaperPath: SPI_GETDESKWALLPAPER failed, GetLastError="
                   << GetLastError();
        return std::wstring();
    }
    return std::wstring(currentWallpaper);
}

void WindowsDesktopWallpaper::RefreshDesktopBackground() {
    // Not under g_desktopStateMutex - this touches none of that shared
    // attach state, only Explorer's own SPI wallpaper mechanism.
    const std::wstring currentWallpaper = GetCurrentWallpaperPath();
    if (currentWallpaper.empty()) {
        qWarning() << "[Wallpaper] RefreshDesktopBackground: could not read the current wallpaper - "
                       "skipping the redraw nudge.";
        return;
    }
    // Re-applying the SAME path the user already has configured - this is
    // strictly a "redraw yourself" nudge to Explorer, never a wallpaper
    // change of our own. SPIF_SENDCHANGE broadcasts WM_SETTINGCHANGE so
    // Explorer picks it up immediately rather than only on next login.
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t*>(currentWallpaper.c_str()),
                                SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) {
        qWarning() << "[Wallpaper] RefreshDesktopBackground: SPI_SETDESKWALLPAPER (redraw nudge) failed, "
                       "GetLastError=" << GetLastError();
        return;
    }
    qInfo() << "[Wallpaper] RefreshDesktopBackground: re-applied Explorer's own current wallpaper "
                "to force it to reclaim/redraw the desktop background layer after we detached.";
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

void WindowsDesktopWallpaper::DumpFullHierarchy(HWND wallpaperHwnd, const char* context) {
    qInfo() << "[Hierarchy]" << context << "=== full desktop hierarchy dump begin ===";

    HWND progman = FindWindowW(L"Progman", nullptr);
    LogWindowDetail(context, "Progman", progman);
    LogChildren(context, "Progman", progman);

    HWND w = nullptr;
    int workerIndex = 0;
    while ((w = FindWindowExW(nullptr, w, L"WorkerW", nullptr)) != nullptr) {
        QString label = QString("WorkerW[%1]").arg(workerIndex);
        LogWindowDetail(context, label.toUtf8().constData(), w);
        LogChildren(context, label.toUtf8().constData(), w);
        ++workerIndex;
    }
    if (workerIndex == 0) {
        qInfo() << "[Hierarchy]" << context << "No top-level WorkerW windows exist right now.";
    }

    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    LogWindowDetail(context, "Shell_TrayWnd", tray);

    // Hypothesis check (read-only, no manipulation): does the desktop icon
    // ListView (SysListView32/"FolderView", the SHELLDLL_DefView descendant
    // that actually owns painting) have its own background image/color set
    // rather than a transparent background? If it paints an opaque
    // full-screen bitmap for its OWN background - which is a documented
    // legacy mechanism (LVM_SETBKIMAGE / LVM_SETBKCOLOR) some Explorer
    // configurations still use to show the classic wallpaper behind icons -
    // that would sit directly ABOVE our window in z-order (SHELLDLL_DefView
    // is immediately above us, confirmed by the z-order-neighbor log below)
    // and fully occlude us even though every Win32/DWM signal for OUR
    // window (visible, not cloaked, correct parent/z-order/size) looks
    // completely correct. All of the messages below are standard
    // cross-process-safe ListView queries (DWORD/COLORREF return values,
    // no pointer marshaling) - purely informational, nothing is changed.
    HWND listView = nullptr;
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            HWND defView = FindWindowExW(hwnd, nullptr, L"SHELLDLL_DefView", nullptr);
            if (defView) {
                HWND lv = FindWindowExW(defView, nullptr, L"SysListView32", nullptr);
                if (lv) {
                    *reinterpret_cast<HWND*>(lParam) = lv;
                    return FALSE;
                }
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&listView));
    if (listView) {
        constexpr UINT kLvmFirst = 0x1000;
        constexpr UINT kLvmGetBkColor = kLvmFirst + 0;
        constexpr UINT kLvmGetTextBkColor = kLvmFirst + 38;
        constexpr UINT kLvmGetExtendedListViewStyle = kLvmFirst + 55;
        constexpr LONG_PTR kLvsExTransparentBkgnd = 0x00800000;
        LRESULT bkColor = SendMessageW(listView, kLvmGetBkColor, 0, 0);
        LRESULT textBkColor = SendMessageW(listView, kLvmGetTextBkColor, 0, 0);
        LRESULT exStyle = SendMessageW(listView, kLvmGetExtendedListViewStyle, 0, 0);
        const bool transparentBkgnd = (exStyle & kLvsExTransparentBkgnd) != 0;
        qInfo() << "[Hierarchy]" << context << "desktop SysListView32 background probe: hwnd="
                << reinterpret_cast<quintptr>(listView)
                << "bkColor=0x" << Qt::hex << (unsigned long)bkColor
                << "textBkColor=0x" << (unsigned long)textBkColor
                << "exStyle=0x" << (unsigned long)exStyle << Qt::dec
                << "LVS_EX_TRANSPARENTBKGND=" << transparentBkgnd
                << "(CLR_NONE=0xFFFFFFFF means no explicit color set; "
                    "transparentBkgnd=false + a real bkColor would mean this ListView paints "
                    "its own opaque background, which sits directly above our window)";
    } else {
        qInfo() << "[Hierarchy]" << context << "Could not locate the desktop SysListView32 for the background probe.";
    }

    // Any other top-level window that plausibly participates in desktop
    // rendering - anything covering a desktop-sized area, or owned by
    // Explorer's PID, or belonging to our own process (should be exactly
    // our wallpaper HWND(s), now popup-turned-child so this normally finds
    // nothing extra - listed for completeness in case reparenting is in an
    // unexpected state). Also specifically flags any class name containing
    // "Wallpaper", "Xaml", "Composition", or "Desktop", in case this
    // Windows build renders the backdrop through a distinct top-level
    // surface neither Progman nor WorkerW.
    DWORD explorerPid = 0;
    if (progman) {
        GetWindowThreadProcessId(progman, &explorerPid);
    }
    struct EnumCtx {
        const char* context;
        DWORD explorerPid;
        DWORD ownPid;
    };
    EnumCtx ctx{context, explorerPid, GetCurrentProcessId()};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = reinterpret_cast<EnumCtx*>(lParam);
            wchar_t className[256] = {};
            GetClassNameW(hwnd, className, 256);
            QString cls = QString::fromWCharArray(className);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            const bool interestingClass = cls.contains("Wallpaper", Qt::CaseInsensitive) ||
                                           cls.contains("Xaml", Qt::CaseInsensitive) ||
                                           cls.contains("Composition", Qt::CaseInsensitive) ||
                                           cls.contains("Desktop", Qt::CaseInsensitive) ||
                                           cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd";
            const bool isOwnProcess = (pid == c->ownPid);
            if (!interestingClass && !isOwnProcess) {
                return TRUE;
            }
            if (cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd") {
                return TRUE; // already logged above individually
            }
            RECT r{};
            GetWindowRect(hwnd, &r);
            const bool deskSized = (r.right - r.left) > 500 && (r.bottom - r.top) > 300;
            QString label = QString("top-level%1%2").arg(isOwnProcess ? " OURS" : "")
                                 .arg(deskSized ? " DESKTOP-SIZED" : "");
            LogWindowDetail(c->context, label.toUtf8().constData(), hwnd);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));

    if (wallpaperHwnd) {
        LogWindowDetail(context, "OUR wallpaperHwnd", wallpaperHwnd);
        HWND parent = GetParent(wallpaperHwnd);
        HWND prev = GetWindow(wallpaperHwnd, GW_HWNDPREV);
        HWND next = GetWindow(wallpaperHwnd, GW_HWNDNEXT);
        qInfo() << "[Hierarchy]" << context << "OUR wallpaperHwnd z-order neighbors: prev(above)="
                << reinterpret_cast<quintptr>(prev) << "next(below)=" << reinterpret_cast<quintptr>(next)
                << "(GW_HWNDPREV is the sibling ABOVE us in z-order, GW_HWNDNEXT is BELOW)";
        LogWindowDetail(context, "sibling ABOVE ours (GW_HWNDPREV)", prev);
        LogWindowDetail(context, "sibling BELOW ours (GW_HWNDNEXT)", next);
        Q_UNUSED(parent);
    }

    qInfo() << "[Hierarchy]" << context << "=== full desktop hierarchy dump end ===";
}

RECT WindowsDesktopWallpaper::GetVirtualDesktopRect() {
    RECT r{};
    r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return r;
}
