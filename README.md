# Motiva

**Live video wallpapers for Windows.** Motiva is a native Windows application
(C++ / Qt 6 / Win32 / DirectComposition) that plays videos, GIFs and image
slideshows **behind your desktop icons**. No Electron, no browser, no fake
fullscreen window.

Current release: **v2.0.0** - see the
[Releases page](https://github.com/guptaji0358/Motiva/releases) for the installer.
Website: the [`website/`](website) folder (static HTML/CSS/JS).

## What's new in v2.0.0

- **GPU-drawn interface.** The main window, Settings, Playlists and every
  utility window are drawn by one OpenGL canvas with an animated background,
  borderless rounded windows and live theming.
- **Playlists.** Image, video and **mixed** playlists, stored in a local
  SQLite library (`.mtv`). Image playlists rotate on unlock, Windows start or
  an interval; video playlists advance when a video ends.
- **Categories.** Per-playlist categories, either rule-based or hand-picked,
  with nesting ("include subcategories"). Activate a category as the wallpaper
  scope.
- **Wallpaper Flow.** A node diagram for reordering a playlist, with undo/redo,
  zoom, keyboard shortcuts and A-Z / Z-A arrangement.
- **Theme editor.** Four built-in themes (Dark/Light Aurora, Dark/Light Onyx)
  plus per-color customization, accent presets and automatic WCAG contrast fixes.
- **Windows Explorer menu.** A cascading *Motiva* right-click menu: set as
  background, add to a playlist, or create a new one.
- **Missing-media recovery.** *Find File* and *Find All at Once* locate moved
  files in a single pass over your drives.
- **Optional local media backup**, off by default, plus a **Cleanup & Reset**
  window (cache, data, factory reset).
- **Unified notifications.** In-app toasts and, when Motiva is in the
  background, Windows notifications; all configurable.
- **Faster, sturdier startup.** The UI appears immediately; desktop attach,
  media restore and Explorer hand-offs run afterwards. A second launch hands
  off to the running instance instead of silently exiting.
- Tabbed / sidebar / grouped **Settings** layouts and a custom Motiva tooltip.

## Features

- Video wallpapers (MP4, MKV, MOV, AVI, WMV, OGV), animated GIFs and still images
- Drag and drop, *Open Video*, or paste a direct http(s) link (`Ctrl+V`)
- All monitors, primary only, or a specific monitor; Fill / Fit / Stretch / Original scaling
- Play, pause, mute, volume and loop controls
- *Show video on battery* setting; images can be kept on battery
- Recovers on its own after an Explorer restart
- Steps aside when you choose a normal wallpaper with *Set as desktop background*
- Start with Windows, Start Menu entry, system tray control
- Everything stays on your PC: no account, no cloud, no telemetry

## How the desktop integration works

All raw Win32 desktop logic lives in
[`WindowsDesktopWallpaper`](scripts/src/WindowsDesktopWallpaper.cpp) and nowhere else.

1. Motiva finds the window that owns the desktop icons (`SHELLDLL_DefView`,
   usually under `Progman`). On current Windows 11 builds no standalone
   icon-less `WorkerW` exists, so the render window attaches to `Progman`
   directly.
2. The render window is a **plain native Win32 window** (not a `QWidget`),
   created with `WS_EX_NOREDIRECTIONBITMAP` and presented through
   **DirectComposition + Direct3D 11** (`D3DWallpaperRenderer`).
3. It is parented to `Progman` and placed in the z-slot **directly behind
   `SHELLDLL_DefView`** with `SetWindowPos(hwnd, shellDefView, ...)`, once, with
   no later z-order upkeep. Icons and the taskbar stay on top.
4. Attach runs on a worker thread so the UI never blocks. Results are discarded
   if an Explorer restart has since moved the *attach generation* on.
5. "Active" is only reported after frames are actually being presented, not
   merely after `SetParent` succeeded.
6. Explorer restarts are detected through the `TaskbarCreated` broadcast and
   recovered automatically. A second launch of Motiva also triggers recovery
   as a safety net.
7. Motiva never changes your wallpaper setting. If Explorer's own wallpaper
   changes while Motiva is active, it detaches so the new picture is visible.
8. A fresh launch never re-attaches on its own: only an explicit
   *Set as Wallpaper* action activates the wallpaper.

If attaching fails, the render window stays hidden instead of becoming a fake
fullscreen window.

## Project layout

```
Motiva/
├── scripts/
│   ├── main/main.cpp        - entry point, single-instance lock, logging
│   ├── src/                 - implementation (UI, playlists, library, wallpaper, Win32)
│   └── include/             - headers
├── resources/               - app.rc, manifest, Qt resources
├── Assets/                  - icons and artwork, organised by feature
├── installer/               - Inno Setup script and build pipeline
├── website/                 - project website
├── CMakeLists.txt
└── README.md
```

Key components: `GlUi` / `GlWindow` / `GlPopups` (GL UI toolkit),
`PlaylistLibrary` / `LibraryDatabase` (SQLite library), `PlaylistDialog`,
`FlowDialog`, `ThemeEditorDialog`, `NotificationManager`, `CleanupManager`,
`BackupManager`, `MediaRecovery`, `WallpaperManager`, `D3DWallpaperRenderer`,
`WindowsDesktopWallpaper`. [`CLAUDE.md`](CLAUDE.md) holds detailed development
notes and history.

## Building

Requires Qt 6 (Widgets, Multimedia, OpenGL, OpenGLWidgets, SQL), the MinGW
toolchain that ships with your Qt install, CMake and Ninja.

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="<path-to-Qt>/6.x/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

> Use the exact MinGW toolchain bundled with Qt (e.g. `<Qt>/Tools/mingw1310_64`),
> not an unrelated MSYS2 install; mixed CRTs cause `__imp___argc` link errors.

Build the installer (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php)):

```powershell
installer\build-installer.ps1
```

This deploys the Qt runtime (including `Qt6OpenGL.dll` and
`Qt6OpenGLWidgets.dll`) and writes `release\MotivaSetup.exe`.

## Settings, storage and privacy

- Preferences: `HKCU\Software\Motiva` (via `QSettings`)
- Playlist library: a `.mtv` SQLite file in your local app data
- Log: `%TEMP%\Motiva.log`
- Nothing leaves your PC. Optional media backups are copied to local app data
  only when you turn them on, and originals are only ever read.

## Known limitations

- Motiva relies on Explorer's undocumented desktop hierarchy, as every Windows
  video-wallpaper tool does. A future Windows update could need a small fix in
  `WindowsDesktopWallpaper.cpp`.
- Developed and tested on Windows 11; multi-monitor setups with different
  resolutions have had limited testing.
- The GL interface has no screen-reader support yet, and there is no
  reduced-motion setting.
- Automatic pause on idle or lock is not implemented.
- Video and wallpaper animation use more power than a static wallpaper; use
  *Show video on battery* to control this on laptops.

## License and credits

Copyright &copy; Robin Gupta. RGS&trade;. Built with Claude.
