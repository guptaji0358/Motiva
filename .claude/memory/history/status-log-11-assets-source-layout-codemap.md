## Assets folder (2026-09-12, created empty)

`Assets/{icons,images,gifs}/` exists at the project root for all future
visual/branding resources. As of creation, a full inspection found **zero
existing visual assets anywhere in the project** - no `.png`/`.jpg`/`.gif`/
`.svg`/`.ico` files, no `.qrc`, nothing embedded in source - so nothing was
migrated. The only icon usage today is `style()->standardIcon(
QStyle::SP_MediaPlay)` for the tray icon in `MainWindow.cpp` (an OS-
provided Qt fallback, not a project file) - a real app icon is still the
open "Planned work" follow-up noted below. Each subfolder currently holds
only a `.gitkeep` placeholder. Not yet wired into CMake/`.qrc` - do that
once real assets actually land here.

## Source layout (2026-09-12 reorganization)

Source now lives under `scripts/`, split by role rather than all in one
`src/` folder:
- `scripts/src/*.cpp` — implementation files.
- `scripts/include/*.h` — headers (declarations only, no `.cpp` needed for
  header-only ones like `StartupDiagnostics.h`).
- `scripts/main/main.cpp` — the ONLY executable entry point (`int main`) in
  the whole project; everything else, even files only the exe uses, is an
  implementation file and stays in `scripts/src/`, not here.

This was a pure file-organization move - `CMakeLists.txt`'s
`target_include_directories(Motiva PRIVATE scripts/include)` is the only
reason plain `#include "Foo.h"` still resolves from every `.cpp`; no
source file's `#include` lines needed to change (none used a path prefix
before or after). No class, behavior, or build flag changed. Old paths in
the dated status entries below (e.g. `src/WindowsDesktopWallpaper.cpp`)
are historical and were left as-is; this Code map section is the
up-to-date reference.

## Code map

- `scripts/include/WindowsDesktopWallpaper.h` / `scripts/src/WindowsDesktopWallpaper.cpp`
  — ALL raw Win32 desktop-attach logic (Progman/WorkerW lookup, SetParent,
  z-order). Nothing else should call these Win32 APIs directly.
- `scripts/include/WallpaperManager.h` / `scripts/src/WallpaperManager.cpp`
  — orchestrates the video player + one `WallpaperWindow` per monitor +
  the attach/health-check loop.
- `scripts/include/WallpaperWindow.h` / `scripts/src/WallpaperWindow.cpp`
  — plain native Win32 window (own `WndProc`), deliberately **not** a
  `QWidget` — a reparented `QWidget`'s `paintEvent` was confirmed to never
  fire after `SetParent` (Qt's QPA doesn't know about the reparent). Raw
  GDI `StretchDIBits` in `handlePaint()`.
- `scripts/include/VideoPlayer.h` / `scripts/src/VideoPlayer.cpp` —
  `QMediaPlayer`/`QVideoSink` decode pipeline, shared (one decode) across
  all monitor windows via `std::shared_ptr<QImage>`.
- `scripts/include/MainWindow.h` / `scripts/src/MainWindow.cpp` — UI +
  tray + in-app preview (normal QWidget, no reparenting, not subject to
  the paintEvent issue above).
- `scripts/include/SettingsDialog.h` / `scripts/src/SettingsDialog.cpp` —
  the settings window (scaling/monitor/volume/mute/loop/start-with-
  Windows), split out of `MainWindow` during the UI redesign.
- `scripts/include/SettingsManager.h` / `scripts/src/SettingsManager.cpp`
  — `QSettings` (registry, `HKCU\Software\Motiva`) for user-facing
  preferences (video path, volume, scaling, etc).
- `scripts/include/RecoveryState.h` / `scripts/src/RecoveryState.cpp` —
  small persistent JSON diagnostic/recovery state, separate from
  SettingsManager (per-user AppData file, not registry) - see the
  2026-09-09 entry above. Never authoritative for live attach state.
- `scripts/include/StartupDiagnostics.h` — header-only process-wide
  startup checkpoint timeline singleton.
- `scripts/include/InstanceIpc.h` / `scripts/src/InstanceIpc.cpp` —
  second-instance recovery hand-off (message-only window + `WM_COPYDATA`),
  separate from the `QSharedMemory` single-instance lock in
  `scripts/main/main.cpp`.
- `scripts/main/main.cpp` — the executable entry point (`int main`):
  logging setup, single-instance lock, autostart flag handling,
  `MainWindow` construction.
