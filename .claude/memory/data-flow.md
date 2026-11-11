# Data flow and lifecycle

Summarised from the code and the verified runs recorded in the history logs.
Function names are the ones in the current sources; re-check before relying on
exact steps.

## 1. Process start

1. `main()` (`scripts/main/main.cpp`): log handler, single-instance lock.
   - Lock held by another process -> send `WM_COPYDATA` via `InstanceIpc`
     (waits briefly for the listener) and exit. Winner's `InstanceIpc` emits
     `recoverRequested` -> `MainWindow::recoverOrActivate()`.
   - `--after-reset` waits for the lock (factory reset restart).
2. `MainWindow` constructor only builds UI, tray, IPC listener and power
   registration. `InstanceIpc` starts listening first.
3. `main()` shows the window, then `beginDeferredStartup()` ->
   `completeStartup()`: media restore, Windows-session playlist startup,
   Explorer hand-offs, wallpaper re-apply, backup start. IPC requests arriving
   before this are queued (`runAfterStartup`).
4. **Cold starts never auto-attach.** `wasWallpaperActive` is persisted but a
   fresh process waits for an explicit click (2026-09-12 decision). Only
   `WallpaperManager::onExplorerRestarted()` and `recoverOrActivate()` continue
   an attach the *same process* already made.
5. If `RecoveryState` shows an unclean previous exit, a summary is logged.
   `RefreshDesktopBackground()` runs once on a worker when nothing is attached.

## 2. Set as wallpaper (the main path)

1. UI button or IPC/`--set-background` -> `MainWindow::onSetWallpaper()`
   (checks `isUsableVideoSource`) -> `WallpaperManager::setWallpaper()`.
2. `setWallpaper()` snapshots Windows' static wallpaper path into
   `m_wallpaperBaselineAtAttach`, emits `wallpaperActivated` (UI: "Applying...").
3. `attachAllWindows()` starts `WindowsDesktopWallpaper::AttachToDesktop()` on a
   QtConcurrent worker (generation-stamped). Desktop hierarchy not ready ->
   300 ms retry timer, bounded.
4. Attach = find Progman/WorkerW (no spawn-message dance unless not yet tried
   this generation), SetParent, z-order behind `SHELLDLL_DefView`.
5. GUI thread receives the bool. Stale generation -> discarded.
6. Success -> `recommitAfterReparent()` and a 700 ms check of
   `presentedFrameCount`. Advancing frames -> `wallpaperVerified` ("Active").
   Not advancing -> `STALLED`/`NOT ADVANCING`, re-arm retry.

## 3. Explorer restart and external changes

- `TaskbarCreated` (and as a fallback a polled check) -> `onExplorerRestarted()`:
  bumps generation, pre-recovery `DumpFullHierarchy`, re-attaches all windows.
  A missed broadcast is recovered by a second launch (IPC path).
- `WM_SETTINGCHANGE` from Explorer's "Set as desktop background" ->
  `onPossibleExternalWallpaperChange()`: if the path differs from the baseline
  while active, `removeWallpaper()` runs (with `m_active=false` set first, which
  breaks the broadcast feedback loop).

## 4. Playlists and rotation

- `PlaylistModel` owns the ordered sequence (`playlist_items.position`).
  `PlaylistRotation` fires on unlock / Windows start / interval and calls
  `advance()`. Video items are sequenced on end-of-media (loop honoured); image
  items advance on triggers.
- Category scope is resolved only in `PlaylistLibrary::categoryItemIds`, so the
  view and wallpaper scope agree.
- All DB writes happen on the GUI thread; the library is closed for cleanup via
  `closeForCleanup()`.

## 5. Explorer context menu

1. Registration (`WindowsShellIntegration`): CLSID key values (extensions,
   playlist list `id|i/v/m|name`, `LastPlaylistId`).
2. `MotivaShell.dll` `IExplorerCommand` reads them, computes state for the whole
   selection (single call; no per-file process).
3. Invoke: write paths to a temp file, start `Motiva.exe --explorer-add <id|0> <file>`.
4. `main.cpp` reads and deletes the temp file, forwards via InstanceIpc; the
   app adds items (`"0"` = new playlist; mixed selection -> Mixed playlist) and
   updates `explorerLastPlaylistId`.
Status: the handler is verified with a COM harness on fake shell items;
real Explorer right-click and the Windows 11 top-level menu are NOT verified.

## 6. Browser extension

1. Content script / background (`extension/`) gathers the media. Chrome launches
   `MotivaBrowserHost.exe` through Native Messaging.
2. Host forwards each compact JSON request as WM_COPYDATA command 5 to the
   running app (starting it with `--autostart` if needed, except `playlists`/`abort`).
3. `MainWindow::onBrowserRequest` handles `playlists`, `chunk`, `abort`, `media`.
   Web files go through `WebMediaCache` into `<AppLocalData>/web-cache/<hash>/`.
   The extension first fetches in the browser (cookies work) and streams base64
   chunks; if that fails, Motiva downloads the URL itself with the page as Referer.
   blob:/data: URLs come from the content script.
4. Replies return as command 6 to the host window passed in WPARAM.

## 7. Cloud (read-only)

`CloudWindow` -> `CloudManager` builds a `CloudPlan` (ordered rclone steps,
run one at a time) -> `RcloneRunner` (`RcloneJob`) runs `rclone.exe` with JSON
logs, filtered to media only. Config is an rclone-encrypted file; the password
comes from `RCLONE_CONFIG_PASS` (DPAPI-protected key in QSettings).

## 8. Notifications

Callers fill `Notification` and call `NotificationManager::notify()` -> settings
gate -> de-dup (30 s / 1.5 s cooldown) -> flood guard (8 per 10 s) -> in-app
`ToastHost` and/or `QSystemTrayIcon::showMessage` (desktop-only events when not
foreground).

## 9. Shutdown

Tray Exit -> `onExitRequested()` (clean-exit flag set in `RecoveryState`, detach,
`RefreshDesktopBackground`). `~WallpaperManager` sets `m_shuttingDown` and waits
for the attach watcher. WM_CLOSE minimises to tray by design.

Related: [[architecture]] · [[modules]] · [[known-issues]]
