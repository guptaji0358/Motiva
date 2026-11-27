# Known issues, limitations and unverified items

Status is as recorded in the history logs and this pass. "Unverified" means no
live check was done on a real Explorer/desktop/account. Re-verify before claiming
a fix.

## Open or unverified (highest impact first)

- **Real Explorer right-click menu** (`MotivaShell.dll`): handler verified only
  with a COM harness on fake shell items. Not verified in live Explorer or the
  Windows 11 top-level menu (needs a signed sparse package). Explorer may hold an
  old DLL until restarted. [status-log-01]
- **Folder context menu**: `CommandStateHandler` plus `ExtendedSubCommandsKey`
  interaction not confirmed in real Explorer. [status-log-01 "folder context menu"]
- **Browser extension in a real browser**: loading it, Chrome's manifest/registry
  discovery, hover button and menus, wallpaper/playlist actions through the bridge,
  and sites that block hotlinking by Referer are NOT verified. HLS/DRM/streaming
  sources are unsupported. [status-log-01 "browser extension"]
- **Cloud**: no real sign-in, listing or download against Google, OneDrive or
  Dropbox; reconnect against a live account is unverified; no cloud tests in the repo.
- **Desktop visibility on the reporting machine**: earlier sessions verified by
  proxies (Win32 state, frame counts, logs). Visual confirmation by screenshot
  is required before calling wallpaper behaviour fixed. A 2026-09-09 investigation
  was BLOCKED (locked session) and never visually confirmed. [status-log-07]
- **Explorer restart reliability**: a `TaskbarCreated` broadcast was once missed
  (recovered via IPC). Rapid restart loops in tests produced misleading results;
  space test cycles. [status-log-08, status-log-09]
- **Rare unexplained process disappearance** after many rapid Explorer restarts
  (no crash report). Not reproduced in later runs; no soak test done. [status-log-10]
- **Startup delay on boot**: the user's real boot-time measurement with the
  instrumented build was never recorded; the dev machine showed ~0.5-0.7 s after
  the fix (one 3.2 s outlier). [status-log-07, status-log-09]
- **Drag and drop**: MIME logic verified with Qt, but a real drag from Explorer or a
  browser was never performed. Multiple dropped files keep only the first valid one
  (no playlist/queue in that path). [status-log-04 "drag & drop"]
- **Theme and small screens**: populated lists, other themes and small window
  sizes for the Cloud and GL windows were not all checked. [status-log-02, 06]
- **Accessibility**: no screen-reader support for GL items; QFileDialog pickers are
  native. [status-log-02]
- **Single-machine scope**: multi-monitor with two displays was never tested.
  [status-log-10]

## Known limitations (by design or platform)

- Cloud is read-only, media-only, one account at a time. Dropbox cannot be limited to read
  scopes by the client; the Dropbox app must be registered with read scopes only.
- Without a configured client ID rclone's shared registration is used, Google included
  (no refusal in `CloudManager`). rclone announced retiring its Google one (recorded 2026-10,
  not re-checked). See `docs/CLOUD_STORAGE.md`.
- Local media backup is off by default; "Factory Reset" does not delete backups.
- Web video from HLS/DRM is not supported; text pages are rejected.
- Cached web files are never auto-pruned (a playlist may use them).
- Dead code found by text search was removed 2026-11-27 (uncalled functions, unused icons); app build
  and six test suites passed, UI not checked live. `DropZoneWidget`, `IconButton` and `CategoryTreePicker`
  no longer exist; only comments mention them.

## Dev-environment gotchas

- OneDrive sync can break builds silently (see [[development-workflow]]).
- Claude desktop is an MSIX package; launched processes see virtualised
  AppData/HKCU. Test real storage via WMI launch.
- UIA automation hangs (~27 s) while a modal dialog is open; combo popups do not
  commit via UIA.
- Never inject synthetic keystrokes into the user's session.

## Memory-level gaps

- The library file name/location and the exact schema-9 table list were not
  re-read; see `LibraryDatabase.cpp` before relying on them.
- History-log mentions of cloud upload/sync/backup pages describe removed features.

## Binary Rain (added 2026-11-18; committed)

- Compiled and engine/config/player unit-tested. The app was NOT driven: Settings page, preview,
  desktop attach and battery suspension are unverified at runtime.
- Not visually verified; no CPU/GPU measurement in the app. Engine only: ~4.7 ms/frame (Dense, 1080p, one core).
- One shared frame at the first target monitor's aspect ratio: monitors with a different aspect
  are Fill/Fit-scaled. No display-change handling (same as the wallpaper engine).
- Preview-only (wallpaper not active) keeps rendering at ~30 fps while loaded, even with the window hidden.
- Settings sliders write QSettings on every change.

## Virtual Desktop Preview (added 2026-11-18/19; committed)

- **Verified** (scratch harness, real classes, 2026-11-18): independent video + Binary Rain sources,
  Esc close, idempotent end, active source leaves the shared player's state unchanged, OS wallpaper path
  unchanged, no window left after abrupt session deletion. The "Virtual Desktop" button was NOT clicked
  in the real app.
- **Windowed mode, sim icons/taskbar/Start/windows, battery pause, multi-monitor, hot-plug: built, NOT run
  in the real app.** Multi-monitor + hot-plug untested (one monitor). Battery check is a 5 s poll, untested on a
  real unplug. Canvas may route keys (Esc/F11/I) to the focused chrome item after a click: unverified.
  Controller window is rebuilt (icon state lost) if its monitor is unplugged.
- Hot-plug is handled only inside the preview (the app itself has none).
- **Unmeasured:** 4K decode, real multi-monitor. Each physical-4K GL window cost ~300-350 MB (measured before
  `AA_ShareOpenGLContexts` was set app-wide in `main.cpp`; the effect of that attribute is unmeasured).
- `D3DWallpaperRenderer::computeUvTransform` delegates to `DesktopScaling::uvTransform` (test: old formulas,
  100 combos); the renderer change was built, not run live.
- Repaint-rate fix (2026-11-18, `VdWallpaperSurface::setFrame` uses `canvas()->QWidget::update()`): scratch harness
  paints ~60/s -> ~30/s for 30 fps input. Not verified: effect on the reported flicker, 4K, multi-monitor,
  chrome + video, F11, hover pixel change.

## Virtual Desktop startup + F11 flicker (fixed 2026-11-19; committed)

- **Measured (scratch harness linking the real `VirtualDesktopWindow`/session + `MOTIVA_VD_DIAG`, 1 monitor, Intel UHD, 125% DPI):**
  an OpenGL window whose rect EXACTLY equals the monitor makes Windows fire `WM_DISPLAYCHANGE` bursts (2 while the HWND is
  created + 3 after show; also the `screenAdded/Removed` re-enumerations seen in Motiva.log). Same window 1 px shorter/taller
  (and a non-GL exact window) -> 0 such messages. Fix: `VdDesktop::coverRect()` = monitor rect + 1 px below the screen
  (covers everything, no sliver); used by the window ctor and `fitToScreen()`.
- **F11 root cause (measured):** `setWindowFlags()` hid the window, destroyed + recreated the HWND (2x `WinIdChange`), then re-showed
  it (~165 ms gap on some toggles) and re-entered exact-size fullscreen (display-change burst). GL context was NOT destroyed.
  Fix: `VirtualDesktopWindow::setWindowed` restyles the live HWND with `QWindow::setFlags()`; order matters (windowed: set geometry
  then flags; fullscreen: flags then geometry) because Qt uses stale frame margins otherwise (client came out 18x47 px small).
  `QWidget::windowFlags()` goes stale; `m_windowed` is authoritative. After: 0 Hide/Show/WinIdChange/WM_DISPLAYCHANGE per toggle.
- **Startup root cause:** (1) `MainWindow::hide()` ran before the ~1.1-1.5 s synchronous GL init, so the bare desktop showed; now the
  window hides on `VirtualDesktopSession::firstFramePresented` (controller's first `frameSwapped`) with a 3 s fallback timer;
  (2) the display-change burst above ~0.4 s after show + swap gaps 250-376 ms. Both gone in the harness (10 starts: 0 WM_DISPLAYCHANGE).
- **Tests:** `VdDesktopTests` covers `coverRect`; opt-in GUI `VdWindowTests` (real desktop + GL, skips otherwise)
  checks F11 keeps HWND/GL context, 0 hide/show, exact geometry; it failed (6 hides) with the old
  `setWindowFlags` path restored. Latest suite totals: [development-workflow](development-workflow.md).
- **NOT verified:** the real Motiva.exe was not driven (user's instance was running; not touched); real video file wallpaper (no sample file; a
  timer source + real Binary Rain `VideoPlayer` source were used); multi-monitor / hot-plug (1 monitor); other GPUs/drivers (NVIDIA/AMD may
  differ); a visible flicker can't be seen from here - results are event logs + screen-capture coverage. Flat partial-coverage plateaus
  (0.67-0.93) seen in some harness runs, not oscillation; attributed to an external overlay, unproven.
- **Diagnostics kept:** `VdDiag` (env `MOTIVA_VD_DIAG=1`, logs `[VdDiag]` lines to Motiva.log: handle/show/hide/resize, GL context,
  swap gaps >150 ms, DISPLAYCHANGE/DWM msgs). Idle when env unset.
- **FIXED 2026-11-19 (built, 3 live runs): deadlock on re-applying a wallpaper while a desktop attach is in flight.** Cause: `AttachToDesktop` (worker thread) held `g_desktopStateMutex` while calling SetWindowPos/SetParent on a GUI-owned HWND; the GUI thread blocked on the same mutex in `DetachFromDesktop`. `DetachFromDesktop` now try_locks and answers only cross-thread sent messages (PeekMessage PM_QS_SENDMESSAGE) while waiting. Two requests 0.2 s apart now finish; not tested with video or Explorer restart.

## Audit 2026-11-21 (code review only; fixes built/tested in the final pass, see perf-intelligence.md)
- `deploy-local` `skip_build` treats every exe as stale after a shared-config change (conservative by design; a real
  build or `allow_stale_artifacts` clears it).
- Open (2026-11-27): `motiva-cloud.ini` exists under `out/` but not `third_party/rclone/`, so a default deploy removes
  it and ships none; the installer force-closes Motiva and has no `[InstallDelete]`; a missing `rclone.exe` only warns.
- Error attribution (later fixes in perf-intelligence.md, committed, not live-verified): an error queued before the
  current load is fenced off (`emitPlayerError`), and a browser apply reacts to playback errors only while its own
  file is selected (`BrowserApplyTracker`). QMediaPlayer errors carry no load identity (Qt 6.12), so attribution of
  event-loop-delivered errors is by order only. Live test still required.
- Open, by code review only (needs live repro): `VideoApplyTelemetry::verified` records `cancelled/superseded_by_load`
  if a later load lands before verification. A trial killed before verification is rolled back and marked bad
  (deliberate policy, indistinguishable from a crash).

Related: [[overview]] · [[decisions]] · [[index]]
