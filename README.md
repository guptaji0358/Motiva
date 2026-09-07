# Video Wallpaper

A native Windows desktop application (C++ / Qt 6 / Win32) that plays an MP4
file continuously **behind the desktop icons**, like a live/video wallpaper.
No Electron, no browser, no fake fullscreen window.

**Status: not yet reliable.** The attach mechanics (finding/creating a host
window, reparenting, sizing) work, but live testing surfaced real bugs: the
video can stay invisible until something nudges Explorer/DWM (e.g. pressing
the Windows key), and when it does appear it can cover the taskbar and
desktop icons instead of sitting behind them. Root cause and current
understanding are tracked in [`CLAUDE.md`](CLAUDE.md) — read that before
touching `WindowsDesktopWallpaper.cpp`.

## How the desktop integration works

All of the Win32-specific logic lives in
[`src/WindowsDesktopWallpaper.h/.cpp`](src/WindowsDesktopWallpaper.cpp) and
nowhere else in the codebase.

### Finding the right window to attach to

1. `Progman` (the desktop process) is sent the undocumented message
   `0x052C` (tried with both known wParam/lParam variants), which asks
   Explorer to spawn a `WorkerW` window as a sibling of itself, hosting the
   desktop icons (`SHELLDLL_DefView`).
2. `EnumWindows` finds the top-level window that currently owns a
   `SHELLDLL_DefView` child (normally `Progman` itself, sometimes a
   `WorkerW`, depending on Explorer build).
3. We look for a **plausibly desktop-sized** (≥200×200px) `WorkerW` sibling
   with no `SHELLDLL_DefView` of its own — the plain window Explorer
   renders directly beneath the icon layer. The size check matters:
   Windows reuses the `WorkerW` window class for small, unrelated utility
   windows (observed at a fixed 166×47px, always invisible) that are not
   part of the desktop at all, and a naive "first WorkerW sibling" search
   can attach into one of those and fail.
4. If no such sibling exists — observed on this Windows 11 25H2 build:
   Explorer never creates a standalone icon-less `WorkerW` via the classic
   message at all — we fall back to attaching directly to the icon owner
   itself (typically `Progman`), which is already correctly sized to the
   desktop.

### Attaching without breaking anything

5. Before `SetParent`, the render window's topmost z-order state is
   explicitly cleared via `SetWindowPos(..., HWND_NOTOPMOST, ...)`, its
   style/ex-style are overwritten outright to `WS_CHILD | WS_VISIBLE` / `0`,
   and the calling thread is temporarily switched to
   `DPI_AWARENESS_CONTEXT_SYSTEM_AWARE` for the call (Explorer's shell
   windows are System-DPI-aware; a Per-Monitor-V2-aware caller hits
   `ERROR_INVALID_PARAMETER` from `SetParent` otherwise).
6. **Z-order placement is the trickiest part, and the one most likely to
   need adjusting on a different Windows build.** When falling back to
   attaching directly into `Progman` (step 4), simply pushing the render
   window to the bottom of Progman's children (`HWND_BOTTOM`) turned out to
   place it *below* whatever this Windows build actually uses to paint the
   classic wallpaper backdrop — the window attached successfully with zero
   API errors, yet rendered completely invisibly. The fix, confirmed by
   testing all three z-order positions:
   - `HWND_BOTTOM` → invisible (below the backdrop paint)
   - top of z-order (SetParent's default) → visible, but covers the icons
   - **`SetWindowPos(hwnd, shellDefViewHandle, ...)`** → visible *and*
     behind the icons

   Using the icon view's own handle as `hwndInsertAfter` places our window
   in the exact z-slot immediately behind the icons, displacing whatever
   used to occupy that slot rather than guessing a fixed position. This is
   the actual fix that makes the wallpaper visible; see
   `AttachToDesktop()`'s comments for the full reasoning.
7. If Explorer restarts (crash, "Restart Explorer" from Task Manager, shell
   update), our window loses its parent. `WallpaperManager` polls every 3
   seconds (`checkWorkerWHealth`) and automatically reattaches.

If attaching ever fails outright, **the render window is kept hidden**
rather than falling back to a normal top-level/fullscreen window. A
visible-but-unattached window would just be the "fake fullscreen wallpaper"
this project explicitly avoids, so failure surfaces as an error message
(with details written to `%TEMP%\VideoWallpaper.log`) instead.

### Rendering pipeline

`WallpaperWindow` is a **plain native Win32 window** (own registered window
class, own `WndProc`) — not a `QWidget`. This was a deliberate, tested
decision: a `QWidget` whose `HWND` gets reparented into `WorkerW`/`Progman`
via raw `SetParent` never actually had its `paintEvent` invoked in testing —
Qt's QPA layer tracks its own idea of a window's top-level/child status and
backing store, none of which are told about a reparent performed behind
Qt's back, and its paint dispatch silently swallowed every `WM_PAINT` as a
result (confirmed with instrumentation: frame-ready notifications and
forced `InvalidateRect`/`UpdateWindow` calls arrived fine, but `paintEvent`
was never called, with or without also calling `QWidget::show()`). Owning
the window class and `WndProc` directly sidesteps that entirely — `WM_PAINT`
is handled with raw GDI (`StretchDIBits`), independent of Qt.

Qt is still used for everything else: decoding (`VideoPlayer`, a
`QMediaPlayer`/`QVideoSink` pipeline) and the in-app preview (`MainWindow`
paints `QImage`s from the same `VideoPlayer` into a `QLabel`, which is a
normal, never-reparented Qt widget and has no such issue).

Only **one** decode pipeline runs regardless of how many monitors are
covered. Each decoded frame is converted to a `QImage` once and shared (via
`std::shared_ptr`) with every per-monitor `WallpaperWindow`, which paints it
scaled to its own monitor rect according to the selected scaling mode
(Fill / Fit / Stretch / Original). This avoids decoding the same video
multiple times for multi-monitor "All monitors" mode.

## Project layout

```
VideoWallpaper/
├── src/
│   ├── main.cpp                     - entry point, tray/autostart flag handling
│   ├── MainWindow.{h,cpp}           - UI + in-app preview
│   ├── VideoPlayer.{h,cpp}          - QMediaPlayer/QVideoSink decode pipeline
│   ├── WallpaperWindow.{h,cpp}      - plain native Win32 render window (one per monitor)
│   ├── WallpaperManager.{h,cpp}     - orchestrates player + windows + Win32 attach
│   ├── WindowsDesktopWallpaper.{h,cpp} - ALL raw Win32 desktop integration
│   └── SettingsManager.{h,cpp}      - QSettings (registry) persistence
├── resources/
│   ├── app.manifest                 - per-monitor DPI awareness
│   └── app.rc
├── CMakeLists.txt
└── README.md
```

## Building

Requires Qt 6 (Widgets, Multimedia, MultimediaWidgets) and a matching MinGW
or MSVC toolchain, plus CMake + Ninja.

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="<path-to-Qt>/6.x/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> Use the exact MinGW toolchain that ships with your Qt install (e.g.
> `<Qt>/Tools/mingw1310_64`), not an unrelated system MinGW/MSYS2 install -
> mixing MinGW runtime versions produces `undefined reference to
> '__imp___argc'`-style link errors because the prebuilt Qt static import
> libraries were built against a specific MinGW CRT.

Deploy the Qt runtime next to the executable for a redistributable build:

```bash
<path-to-Qt>/6.x/mingw_64/bin/windeployqt6.exe --release build/VideoWallpaper.exe
```

## Settings & autostart

All settings (video path, volume, mute, loop, scaling, monitor selection,
autostart) are stored via `QSettings` in
`HKCU\Software\VideoWallpaper\VideoWallpaper` - no account, no cloud, no
network access required.

"Start with Windows" writes/removes a value under
`HKCU\Software\Microsoft\Windows\CurrentVersion\Run` pointing at the exe
with a `--autostart` flag; on autostart the app restores the previously
selected wallpaper directly to the tray without showing the main window.

## What was verified during development

Built and iterated directly against a real Windows 11 (build 26200 / 25H2)
machine — a very recent build whose desktop compositing differs from older
Windows 10/11 releases in ways that broke the classic technique's usual
assumptions and required real debugging (not just following a known
recipe):

- The app builds and links cleanly, launches, loads and probes a real MP4
  (h264/aac, 1080p) via the ffmpeg-backed Qt Multimedia pipeline.
- The render pipeline (native `WndProc` + `StretchDIBits`) was verified to
  paint real video frames correctly, isolated from the desktop-attach logic
  entirely, before diagnosing the attach issue itself.
- `SetParent` into a decoy `WorkerW`, DPI-awareness mismatches, and
  leftover Qt window-style bits were each found and fixed (see git history
  / code comments in `AttachToDesktop()`).
- The final, confirmed-working fix: attaching into `Progman` and placing
  the render window's z-order immediately behind the icon view
  (`SHELLDLL_DefView`) via `SetWindowPos(hwnd, shellDefViewHwnd, ...)`,
  rather than `HWND_BOTTOM`. **Confirmed visually via screenshot**: video
  plays as the desktop background with every icon fully visible on top.
- **Still worth doing:** test with 2+ monitors of different resolutions,
  restart `explorer.exe` while the wallpaper is active and confirm
  `checkWorkerWHealth` reattaches correctly (it currently checks
  `GetParent()` validity but does not yet re-resolve the
  `shellDefView`-relative z-order target - worth double-checking after a
  real Explorer restart), and exercise Remove Wallpaper to confirm the
  normal desktop is restored cleanly.

## Known limitations

- The undocumented `0x052C` WorkerW technique is what every known video
  wallpaper tool on Windows uses (there is no public API for this); it has
  been stable since Windows 7 but is not a documented contract. This
  project's own testing already found one real behavioral difference on a
  very recent Windows 11 build (no standalone icon-less `WorkerW` ever
  gets created, and the naive "bottom of z-order" placement is invisible)
  - a future Windows update could change this further and require another
    small fix to `WindowsDesktopWallpaper.cpp`. If the wallpaper ever goes
    invisible again despite `AttachToDesktop` reporting success, re-check
    the z-order placement first (see the reasoning in step 6 above) before
    assuming it's a rendering bug.
- Explorer restart recovery is polling-based (3s interval) rather than
  event-driven, since there is no notification API for "WorkerW
  recreated".
- "Original" scaling centers the video at native resolution but does not
  currently offer a way to pan a video larger than the monitor.
- Automatic pause-on-idle (to save CPU/GPU when e.g. the workstation is
  locked) is not implemented; play/pause is currently only user- and
  tray-triggered.
