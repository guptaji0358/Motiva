# CLAUDE.md

Guidance for Claude Code (or any future agent) working in this repo.

## What this is

A native Windows C++/Qt6 app (`src/`) that plays an MP4 as a live desktop
wallpaper (behind icons, above the OS wallpaper backdrop) using the
undocumented `Progman`/`WorkerW` attach technique. No Electron, no fake
fullscreen overlay.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="<path-to-Qt>/6.x/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Must use the MinGW toolchain that ships with the Qt install (e.g.
`<Qt>/Tools/mingw1310_64`), not a system MinGW/MSYS2 — mismatched CRTs
produce `undefined reference to '__imp___argc'` link errors.

Run/test: `build/VideoWallpaper.exe`. Logs go to
`%TEMP%\VideoWallpaper.log` (via `qInstallMessageHandler` in `main.cpp`) —
always check this file first when diagnosing desktop-attach issues, since
the app has no visible console.

Kill stray instances before rebuilding — a single-instance `QSharedMemory`
lock exists (`main.cpp`), but a running exe still locks the `.obj`/`.exe`
during `cmake --build`:
```bash
taskkill //F //IM VideoWallpaper.exe
```

## Current status (2026-09-07, DirectComposition rewrite)

Presentation was rewritten from GDI (`WM_PAINT`/`StretchDIBits`) to
DirectComposition + D3D11 (`D3DWallpaperRenderer`), driven by a live
diagnostic finding: `Progman` on this machine carries
`WS_EX_NOREDIRECTIONBITMAP`, meaning it opts out of classic DWM
bitmap-redirection compositing - which is almost certainly why a
geometrically-correct GDI child window (verified via `GetWindowRect`/
`IsWindowVisible`, not just a screenshot) never actually rendered. The
render window now carries the same extended style and presents through a
DirectComposition visual instead.

The top-level-sibling-behind-Progman fallback (from the previous session)
was removed entirely - live diagnostics proved it placed the window
**above** Progman/icons in the global z-order (the opposite of what was
wanted) and required periodic z-order upkeep that caused flicker. The app
now always attaches as a real child of Progman, positioned behind
`SHELLDLL_DefView`, exactly once, with no further HWND manipulation during
playback (verified via extended log observation - zero repeated
reattach/SetWindowPos activity over 2+ minutes of runtime).

A custom standalone diagnostic tool (`diag.exe`, built from
`diag.cpp` in the session scratchpad, not part of this repo) confirmed via
live `EnumChildWindows` that after attaching, Progman's children are
ordered front-to-back: `SHELLDLL_DefView` (icons) → our render window →
a genuine, full-desktop-sized, **visible** `WorkerW` that Explorer created
as a **child of Progman** (not the traditional top-level sibling the
existing `FindWorkerWBehindIcons` search logic looks for - a likely
follow-up: teach that search to also check for a WorkerW nested under
Progman, not just top-level siblings). That ordering - icons in front,
video in the middle, backdrop WorkerW at the back - is exactly the
layering a wallpaper needs.

**What is NOT yet visually verified** (this tool session has no way to see
the live desktop - only screenshots/Task Manager reports from the user
establish ground truth):
- Whether the video actually renders on screen now (the architecture and
  z-order are right per diagnostics; DirectComposition initialization
  logs no errors; but "renders" has been wrong before despite correct
  structure - it must be confirmed by an actual screenshot).
- Icon/taskbar interactivity, Win-key behavior, playback smoothness,
  Explorer-restart recovery, multi-monitor (this machine only ever showed
  one 1536x864 or 1920x1080 display across sessions - never tested with
  two).
- Clean shutdown via the tray "Exit" menu item specifically - this tool
  session cannot click a system tray context menu; WM_CLOSE was confirmed
  to correctly minimize-to-tray (by design), which is not the same test.

## Prior status (2026-09-07, pre-DirectComposition) — partially fixed, needs live re-verification

A second pass fixed several concrete, verified-in-code bugs (see git log for
the full diagnosis/fix commit): the GUI-thread frame conversion and slow
HALFTONE blit (playback smoothness/lag), the tray-Exit path skipping
cleanup (process lingering after close), and — critically — a regression
where the WorkerW-discovery probe (several seconds of Sleep-based waiting)
was being re-run by the 3s health-check timer on every tick once it always
failed, making the whole app go "Not Responding" indefinitely. That's now
cooldown-gated to once per 5 minutes.

**Still not fixed / needs your live verification:**
- The WorkerW-discovery probe still runs *synchronously on the GUI thread*
  the (now rare) times it does run - it should be moved to a background
  thread with only the final `SetParent`/`SetWindowPos` marshaled back,
  per the user's explicit "don't block the GUI thread" requirement. Not
  done yet due to time constraints in that session.
- Video-appears-only-after-Win-key and taskbar/icons-covered-while-visible
  (issues D/E/F) were **not** re-verified after these fixes - the WorkerW
  discovery message-toggle fix might have changed nothing on this specific
  machine (it still falls back to Progman every time in testing). Get a
  fresh screenshot and Task Manager check before assuming these are fixed.
- Playback smoothness improvements (moving conversion off the GUI thread,
  faster blit mode) are implemented and build cleanly but were not visually
  verified - this tool session cannot see the interactive desktop.

## Prior status (2026-09-07, original) — NOT reliably working

Despite an earlier README claiming "confirmed working end-to-end", live
testing on the actual dev machine (Windows 11, build `10.0.26200`, an
unusually high/recent — likely Insider/Dev-channel — build number) surfaced
real, reproducible bugs. **Do not trust old status claims in README.md
without re-verifying on the current machine; update them as bugs are
found/fixed.**

Root cause: this Explorer build never spawns a real, DWM-recognized
icon-less `WorkerW`, even after the standard fix of forcing Explorer to
rebuild its desktop pipeline via `SystemParametersInfoW(SPI_SETDESKWALLPAPER,
...)` before retrying (see `WindowsDesktopWallpaper::FindOrCreateWorkerW`).
All `WorkerW`-class windows found on this system are 166×47 decoys unrelated
to the desktop.

Without a genuine WorkerW, `AttachToDesktop()` falls back to a top-level
sibling window pinned behind `Progman` in the global z-order
(`isProgmanFallback` branch). This fallback is unstable:

- **Bug 1**: video stays invisible until something (observed: pressing the
  Windows key) nudges Explorer/DWM into recompositing.
- **Bug 2**: was periodic flicker from `checkWorkerWHealth()` unconditionally
  re-attaching every 3s tick; fixed to only re-pin when the window has
  actually drifted out of position (`GetWindow(hwnd, GW_HWNDPREV) != progman`).
  Re-verify this actually eliminated the flicker on real hardware.
- **Bug 3**: taskbar and desktop icons get covered while the video is
  visible — consistent with the window occasionally getting promoted above
  everything (not just above the backdrop) rather than staying wedged
  between backdrop and icons, since there's no stable "wedge" z-slot without
  a real WorkerW.

All three point at the same underlying issue: on this Explorer build there
is no reliable middle z-position between the OS wallpaper backdrop and the
desktop icons/taskbar for a foreign window to occupy. This may be a genuine
platform limitation of this specific (possibly Insider) Windows build rather
than something fixable purely in application code — next session should
re-test after Windows updates, and/or investigate whether a newer,
documented API (e.g. anything under `IShellWallpaperHost` or similar,
introduced in recent Windows 11 builds) exists as a replacement for the
`0x052C` hack before sinking more time into z-order guessing.

Diagnostic pattern that worked well: temporarily add `qWarning()` calls
dumping `GetWindowRect`/`GetClientRect`/`IsWindowVisible`/`GetParent` around
`AttachToDesktop`, rebuild, run, read `%TEMP%\VideoWallpaper.log` — this
tool session cannot see the interactive desktop directly (`FindWindow`
returns null from this shell's session), so screenshots from the user are
the only way to visually verify rendering.

## Code map

- `src/WindowsDesktopWallpaper.{h,cpp}` — ALL raw Win32 desktop-attach logic
  (Progman/WorkerW lookup, SetParent, z-order). Nothing else should call
  these Win32 APIs directly.
- `src/WallpaperManager.{h,cpp}` — orchestrates the video player + one
  `WallpaperWindow` per monitor + the attach/health-check loop.
- `src/WallpaperWindow.{h,cpp}` — plain native Win32 window (own `WndProc`),
  deliberately **not** a `QWidget` — a reparented `QWidget`'s `paintEvent`
  was confirmed to never fire after `SetParent` (Qt's QPA doesn't know about
  the reparent). Raw GDI `StretchDIBits` in `handlePaint()`.
- `src/VideoPlayer.{h,cpp}` — `QMediaPlayer`/`QVideoSink` decode pipeline,
  shared (one decode) across all monitor windows via `std::shared_ptr<QImage>`.
- `src/MainWindow.{h,cpp}` — UI + tray + in-app preview (normal QWidget, no
  reparenting, not subject to the paintEvent issue above).
- `src/SettingsManager.{h,cpp}` — `QSettings` (registry, `HKCU\Software\VideoWallpaper`).
