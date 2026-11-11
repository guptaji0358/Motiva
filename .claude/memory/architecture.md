# Architecture

Verified against `CMakeLists.txt`, `scripts/main/main.cpp` and the headers in
`scripts/include/`. Behaviour details are in [[data-flow]]; classes in [[modules]].

## Build targets (CMakeLists.txt)

| Target | Kind | Source | Role |
|---|---|---|---|
| `Motiva` | WIN32 Qt exe | `scripts/main/main.cpp` + `scripts/src/*.cpp` + `resources/app.rc/.qrc` | The application |
| `MotivaShell` | SHARED DLL | `scripts/shell/MotivaShell.cpp` | Qt-free COM `IExplorerCommand` (Explorer menu); per-user, loaded by Explorer |
| `MotivaBrowserHost` | plain (console) exe | `scripts/browser/MotivaBrowserHost.cpp` | Native Messaging relay for the browser extension; Qt-free, links user32 only |
| `MotivaLauncher` | WIN32 exe, `EXCLUDE_FROM_ALL` | `scripts/main/launcher.cpp` | Used only by the deploy step (`deploy/Deploy.ps1`) |

Qt modules linked: Widgets, OpenGL, OpenGLWidgets, Multimedia,
MultimediaWidgets, Concurrent, Svg, Sql, Network. C++17.

## Layers inside `Motiva`

1. **Entry & process**: `main.cpp` — logging to `%TEMP%\Motiva.log`, the
   `QSharedMemory` single-instance lock, CLI flags, `StartupDiagnostics`
   checkpoints, second-instance hand-off via `InstanceIpc`.
2. **Shell window / UI**: `MainWindow` (borderless, GL-drawn header/preview,
   tray), `SettingsDialog`, `PlaylistDialog`, `CloudWindow`, `CleanupWindow`,
   `BackupWindow`, `FlowDialog`, `ThemeEditorDialog`, media-recovery dialogs.
   Shared GL toolkit: `GlUi` (canvas + items), `GlWindow` (utility-window
   base), `GlPopups` (`GlMenu`, `GlMessageBox`), `GlColor`.
3. **Domain / orchestration**: `WallpaperManager` (attach, verify, Explorer
   restart, generations), `PlaylistModel` / `PlaylistRotation` /
   `PlaylistLibrary`, `CloudManager` + `RcloneRunner`, `BackupManager`
   (+ `LocalBackupProvider`), `CleanupManager`, `WebMediaCache`,
   `NotificationManager`, `RecoveryState`.
4. **Persistence**: `SettingsManager` (QSettings in HKCU), `LibraryDatabase`
   (SQLite `.mtv`, schema migrations up to v9), `RecoveryState` (JSON in
   AppData, diagnostic only).
5. **Rendering / media**: `VideoPlayer` (QMediaPlayer + QVideoSink, one decode
   shared by all monitors via `shared_ptr<QImage>`), `D3DWallpaperRenderer`
   (DComp + D3D11 presentation), `WallpaperWindow` (plain Win32 HWND, not a
   QWidget), `MediaPreview`.
6. **Win32 boundary**: `WindowsDesktopWallpaper` (Progman/WorkerW, SetParent,
   z-order) — the ONLY place raw desktop-attach Win32 is called;
   `WindowsShellIntegration` (registry/Explorer verbs, Explorer playlist menu
   data); `BrowserIntegration` (native-messaging manifests, HKCU keys).

## Threading rules (enforced by convention, see [[decisions]])

- GUI thread owns all SQLite connections and all Qt widgets/GL items.
  Workers do file I/O only.
- `WallpaperManager` runs `AttachToDesktop` on a QtConcurrent worker and
  marshals only the boolean back; a `std::mutex` guards the module globals in
  `WindowsDesktopWallpaper.cpp`. There is exactly one `AttachToDesktop` call
  site (`WallpaperManager`).
- `notify()` is thread-safe (queued to GUI thread).
- Render thread presents frames; GUI thread never blocks on `Present()`
  (the old blocking rebind caused a deadlock — see history log 10).

## Process model

- One interactive process at a time (`QSharedMemory` lock).
- A second launch sends `WM_COPYDATA` to `InstanceIpc`'s message-only window
  and exits; the running instance may recover or activate.
- The browser host and Explorer shell DLL talk to the running app over the
  same `InstanceIpc` window (commands 4/5/6) or via a temp file for Explorer
  (`--explorer-add`).

## Storage locations

- Settings: `HKCU\Software\Motiva` (QSettings).
- Library: `.mtv` SQLite under the app data folder (name/location in
  `LibraryDatabase`/`PlaylistLibrary`; not re-verified this pass).
- Recovery state: `%LOCALAPPDATA%\...\recovery-state.json` (QStandardPaths AppData).
- Web cache: `<AppLocalData>/web-cache/<hash>/<name>`.
- Local media backups: `AppLocalDataLocation/backups/objects/` (SHA-256 names).
- Cloud rclone config: encrypted `rclone.conf`; key under DPAPI in QSettings
  `cloud/rcloneKey`.
- Log: `%TEMP%\Motiva.log`.
