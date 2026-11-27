# Technology stack

Sources: `CMakeLists.txt`, `build-and-deploy.bat`, `scripts/extension/manifest.json`,
README and the history logs. Versions are as recorded in the repo's own scripts.

## Application (Motiva.exe)

- **Language**: C++17 (`CMAKE_CXX_STANDARD 17`, required).
- **Build**: CMake + Ninja, **MinGW-w64 toolchain shipped with Qt** (e.g.
  `C:\Qt\Tools\mingw1310_64`). Mixing with a system MinGW/MSYS2 causes
  `undefined reference to '__imp___argc'`.
- **Qt**: Qt 6.12.0 (verified 2026-11-26: installed kit is `C:\Qt\6.12.0\mingw_64`; `D:\Apps\Qt6` is gone).
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
- Files: `background.js` (service worker), `content.js` (hover button, closed shadow DOM host mounted on <html>, not <body>),
  `popup.html`/`popup.js`, `compat.js` (resolution compatibility, loaded before
  `content.js`).
- Resolution-Based Compatibility (verified with node on `compat.js` only; not
  yet tried in a live browser): hover button is hidden unless the media, drawn
  cover-style on the window's display (`screen.*` x `devicePixelRatio`), has
  scale <= 1.25 and crop <= 30%. Setting `resolutionCompat` in
  `chrome.storage.local` (default on), toggled in the extension popup, not in
  Motiva's Qt settings. Image `naturalWidth/Height`, video `videoWidth/Height`.
  Button state is derived by `syncButton()` in `content.js` (mousemove throttled
  to an 80 ms window with one pending trailing timer owned by `runTrail()`;
  measured ~12 syncs/s under continuous (~60 Hz) or rapid CDP pointer movement in
  headless Chrome (HEAD before the fix: ~42-53/s); plus `load`/`loadedmetadata`/`emptied`/video `resize`
  re-check, a `MutationObserver` (img/video add/remove, src/srcset/poster
  changes; ignores the extension's own host; disconnects on pagehide/orphaning,
  restarts on bfcache pageshow) and DOMContentLoaded/load syncs; all go through
  one debounced `scheduleSync`. Pointer position is only known after the first
  mousemove (no sync without it). Stationary-pointer dynamic insert, src swap,
  lazy src and removal verified on a shimmed page with synthetic mousemove
  (real videos not tested); verified
  in the browser pane with a shimmed `chrome.*` page: compatible shows,
  incompatible hides, fast move and src swap under a still pointer update.
  Verified 2026-11-13 in real Chrome 157 (1920x1080 @125%, Extensions.loadUnpacked via CDP,
  needs --enable-unsafe-extension-debugging; --load-extension is ignored): image/video
  compatible shows, incompatible hides, toggle off shows. Pre-fix HEAD behaved the same
  here, so a user-side hide-all was NOT reproduced; use debugCompat to capture it.
- Source-aware states (`content.js` `sourceSize()`/`assess()`; verified by node
  tests only, NOT yet in real Chrome): size = decoded natural size or a larger
  `srcset` candidate (`w`, or `x` * rendered width; no requests, parse cached
  per srcset string). Compatible -> normal button; known-but-failing image ->
  button with `low` class ("low-res" marker), `send` carries `lowRes`; unknown
  size or incompatible video -> hidden. Thresholds unchanged (1.25 / 0.3).
- Visibility rule is `MotivaCompat.decide(enabled, media, display)`: setting off
  -> shown; unusable display size (`screen.*` = 0, seen in the hidden browser
  pane) -> `display-unknown`, shown (previously this hid EVERYTHING); unknown
  media size -> hidden until `load`/`loadedmetadata`. Debug: set
  `chrome.storage.local.debugCompat = true` to log state/media/display/dpr in
  the page console, plus a `[Motiva stage]` line (placed / hidden-by-compat /
  no-eligible-media + top-5 `elementsFromPoint` with reject reasons). Tests: `node --test scripts/extension/tests` (`compat.test.js`
  evaluator; `content.test.js` runs `content.js` in a fake-DOM vm harness).
- Visibility state (2026-11-13, verified by `content.test.js`, not in a live
  browser): `syncButton()` owns visibility; every branch ends shown-on-`current`
  or `hideAll()` (button+menu hidden, `current=null`). Overlay off, orphaned
  script or no pointer -> hide. Menu open or pointer on the button keeps state
  only while `currentValid()` (connected, same src, compatible). `send()`
  re-checks it. Overlay toggle in storage calls `syncButton()`. Scroll hides at
  once, one 120ms-debounced re-sync at the stationary pointer. `mouseout` with
  null `relatedTarget` clears pointer/media state and hides.
  Eligibility (verified on Google Images grid): rendered short side >= 80px and
  long side >= 120px (was both >= 120, which rejected 200x103 grid thumbnails;
  those are data: URIs ~246x138 natural, shown with the low-res marker).

## Packaging

- `scripts/deploy/Deploy.ps1` → `out/deployment/` (launcher as `Motiva.exe`, real app in
  `resources/bin/`, `windeployqt` output moved into `resources/Qt` and `plugins`).
- `scripts/installer/MotivaSetup.iss` (Inno Setup) → `out/release/MotivaSetup.exe`
  (`scripts/installer/build-installer.ps1`). Installer and any MSIX file list must include
  the Qt OpenGL DLLs.
- Explorer verbs/COM DLL are registered at runtime (`WindowsShellIntegration`),
  not by the installer alone.

## Not used (verified absent)

No Electron, no Node build. No OAuth/token library in Motiva itself. Tests are opt-in only: QtTest targets in
`scripts/tests/` and `node --test scripts/extension/tests`; other verification is harnesses and live runs.

Related: [[development-workflow]] · [[architecture]] · [[decisions]]
