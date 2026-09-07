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

## Current status (2026-09-07) — NOT reliably working

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
