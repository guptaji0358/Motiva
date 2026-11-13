# Technology stack

Sources: `CMakeLists.txt`, `build-and-deploy.bat`, `extension/manifest.json`,
README and the history logs. Versions are as recorded in the repo's own scripts.

## Application (Motiva.exe)

- **Language**: C++17 (`CMAKE_CXX_STANDARD 17`, required).
- **Build**: CMake + Ninja, **MinGW-w64 toolchain shipped with Qt** (e.g.
  `D:\Apps\Qt6\Tools\mingw1310_64`). Mixing with a system MinGW/MSYS2 causes
  `undefined reference to '__imp___argc'`.
- **Qt**: Qt 6.12.0 (verified 2026-11-12: installed kit is `D:\Apps\Qt6\6.12.0\mingw_64`, and `build/CMakeCache.txt` points there). `build-and-deploy.bat` line 4 was corrected from the missing 6.11.1 to 6.12.0 on 2026-11-13.
  Components: Widgets, OpenGL, OpenGLWidgets, Multimedia, MultimediaWidgets,
  Concurrent, Svg, Sql, Network. Qt6OpenGL.dll and Qt6OpenGLWidgets.dll must be
  deployed.
- **Video**: Qt Multimedia (FFmpeg backend) — `QMediaPlayer` + `QVideoSink`;
  web video URLs are streamed directly by `QMediaPlayer` (no custom networking).
- **Graphics**: Direct3D 11 + DirectComposition (`D3DWallpaperRenderer`) for the
  wallpaper; OpenGL (`QOpenGLWidget` `GlCanvas`) for the main UI, with QPainter
  fallback when shaders fail.
- **Win32 APIs**: Progman/WorkerW discovery, `SetParent`/`SetWindowPos`,
  `SystemParametersInfo`, DWM rounded corners, `WM_COPYDATA` IPC, DPAPI
  (`CryptProtectData`, cloud key), `RegisterClass`/shell extensions.
- **Persistence**: QSettings (registry), Qt SQL (SQLite `.mtv` library),
  QStandardPaths for JSON recovery state.
- **Icons/SVG**: Qt SVG (`QSvgRenderer`), `Theme::tintedIcon()` for recoloured SVG.

## Companion binaries

- **MotivaShell.dll** — Qt-free COM (`ole32`, `shell32`, `uuid`), static runtime.
- **MotivaBrowserHost.exe** — Qt-free, links `user32`, static runtime; Native
  Messaging stdin/stdout framing; compact JSON required (naive `"key":"` scan).
- **MotivaLauncher.exe** — deploy-only, `Shell32 User32`, `-municode`.
- **rclone 1.75.2** (MIT) bundled in `resources/bin/rclone/` for cloud; min
  accepted version 1.65 (checked at runtime). Branded by
  `scripts/tools/brand_rclone_exe.py`.

## Browser extension

- Manifest V3, `minimum_chrome_version` 110, permissions `contextMenus`,
  `nativeMessaging`, `storage`; `host_permissions` `<all_urls>`.
- Fixed extension ID through the `key` field; same ID in
  `BrowserIntegration::kExtensionId` and the host's origin check.
- Files: `background.js` (service worker), `content.js` (hover button, shadow DOM),
  `popup.html`/`popup.js`, `compat.js` (resolution compatibility, loaded before
  `content.js`).
- Resolution-Based Compatibility (verified with node on `compat.js` only; not
  yet tried in a live browser): hover button is hidden unless the media, drawn
  cover-style on the window's display (`screen.*` x `devicePixelRatio`), has
  scale <= 1.25 and crop <= 30%. Setting `resolutionCompat` in
  `chrome.storage.local` (default on), toggled in the extension popup, not in
  Motiva's Qt settings. Image `naturalWidth/Height`, video `videoWidth/Height`.
  Button state is derived by `syncButton()` in `content.js` (throttled move
  with trailing run, plus `load`/`loadedmetadata`/`emptied` re-check); verified
  in the browser pane with a shimmed `chrome.*` page: compatible shows,
  incompatible hides, fast move and src swap under a still pointer update.
  Real Chrome load of the unpacked extension: not verified.

## Packaging

- `deploy/Deploy.ps1` → `deployment/` (launcher as `Motiva.exe`, real app in
  `resources/bin/`, `windeployqt` output moved into `resources/Qt` and `plugins`).
- `installer/MotivaSetup.iss` (Inno Setup) → `release/MotivaSetup.exe`
  (`installer/build-installer.ps1`). Installer and any MSIX file list must include
  the Qt OpenGL DLLs.
- Explorer verbs/COM DLL are registered at runtime (`WindowsShellIntegration`),
  not by the installer alone.

## Not used (verified absent)

No Electron, no Node build, no test framework in the repo (verification has been
out-of-tree harnesses and live runs). No OAuth/token library in Motiva itself.

Related: [[development-workflow]] · [[architecture]] · [[decisions]]
