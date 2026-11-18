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
- **Cloud**: no real sign-in, listing or transfer against Google, OneDrive or
  Dropbox. Reconnect against a live account is unverified. Cloud window pages
  may still reference removed features. `docs/CLOUD_STORAGE.md` still describes
  the removed sync/backup features.
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

- Cloud is read-only and media-only. Dropbox cannot be limited to read scopes by the
  client; the Dropbox app must be registered with read scopes only.
- Google needs the user's own OAuth client ID (rclone's shared one is retired).
- Local media backup is off by default; "Factory Reset" does not delete backups.
- Web video from HLS/DRM is not supported; text pages are rejected.
- Cached web files are never auto-pruned (a playlist may use them).
- The `scripts/superseded-oauth-cloud/` folder is dead code kept for reference.
- Dead code: `DropZoneWidget`, `IconButton`,
  `CategoryTreePicker` are compiled but unused; `NeedsReattach` is never called.
  [status-log-02 "still compiled", status-log-07]

## Dev-environment gotchas

- OneDrive sync can break builds silently (see [[development-workflow]]).
- Claude desktop is an MSIX package; launched processes see virtualised
  AppData/HKCU. Test real storage via WMI launch.
- UIA automation hangs (~27 s) while a modal dialog is open; combo popups do not
  commit via UIA.
- Never inject synthetic keystrokes into the user's session.

## Memory-level gaps

- The library file name/location and the exact schema-9 table list were not
  re-read in this pass; see `LibraryDatabase.cpp` before relying on them.
- `CloudManager` lines in the cloud sections of the history may describe pages
  that the 2026-10-24 policy removed.

## Binary Rain (added 2026-11-18; uncommitted)

- **Compiled** (2026-11-18, scratch build with Qt kit at `C:\Qt\6.12.0\mingw_64`, Qt Multimedia installed): the
  full `Motiva` target builds with no errors. The app was NOT launched (user's PC in use), so the Settings page,
  preview, desktop attach and battery suspension are unverified at runtime.
- Not visually verified; no CPU/GPU measurement in the app. Engine only: ~4.7 ms/frame (Dense, 1080p, one core).
- One shared frame at the first target monitor's aspect ratio: other monitors with a different aspect
  are Fill/Fit-scaled. No display-change handling (same as the rest of the app).
- Preview-only (wallpaper not active) keeps rendering at ~30 fps while loaded, even with the window hidden.
- Settings sliders write QSettings on every change.

## Virtual Desktop Preview (Phase 1, added 2026-11-18; uncommitted)

- **Verified** (scratch harness `SessionSmoke`, real classes, 2026-11-18): independent video + Binary Rain
  sources, Esc close, idempotent end, active source leaves the shared player's state unchanged, OS wallpaper
  path unchanged, no window left after abrupt session deletion; `DesktopScalingTests` 9 passed; full `Motiva`
  target builds. The "Virtual Desktop" button was NOT clicked in the real app (UI not driven).
- **Phases 3-4 (windowed, sim icons/taskbar/Start/windows, battery pause, multi-monitor, hot-plug): built, NOT run
  in the real app** (Motiva builds; `VdDesktopTests` 13 pass). Multi-monitor + hot-plug untested: dev laptop has
  one monitor. Battery check is a 5 s poll; untested on a real unplug. Canvas may route keys (Esc/F11/I) to the
  focused chrome item after a click: unverified. Controller window is rebuilt (icons state lost) if its monitor
  is unplugged.
- **Not built yet:** hot-plug handling outside the preview (the app itself has none).
- **Unmeasured:** 4K decode, real multi-monitor. Phase 0: each physical-4K GL window costs ~300-350 MB;
  GPU draw ~33% of a core vs 130-150% for CPU pre-scale at 30 fps. `AA_ShareOpenGLContexts` was not set then (set since Phase 5).
- `D3DWallpaperRenderer::computeUvTransform` now delegates to `DesktopScaling::uvTransform` (Phase 5); a test
  checks it against the old formulas (100 combos). The renderer change itself was built, not run live.
- Phase 5: `Qt::AA_ShareOpenGLContexts` is now set in `main.cpp` (app-wide). Its memory/CPU effect and any
  regression in the main window or dialogs are UNMEASURED/unverified (no multi-monitor machine, app not driven).

- **Repaint-rate fix (2026-11-18, uncommitted):** `VdWallpaperSurface::setFrame` now calls
  `canvas()->QWidget::update()` instead of `GlItem::update()` (which re-armed `GlCanvas`'s 16 ms hot-frame
  timer, so video/rain repainted ~60/s for ~30 fps input). Measured in a scratch harness (one 60 Hz monitor,
  Intel UHD, 1080p; not in the repo): video paints ~56-60/s -> ~28-32/s, rain ~60 -> ~30, timer ticks ~55 -> 0-4;
  static previews idle at 0 paints both before and after; no wrong-thread calls; last synthetic frame rendered
  correctly; hover/button animation scheduling identical. Paint cost ~1 ms. Unit tests (59) pass.
  **Not verified:** that this removes the reported full-screen flicker (never observed); 4K, multi-monitor,
  chrome-visible + video, F11 toggle; hover pixel change (harness check was inconclusive, 0 diff in baseline too).
  Other flicker hypotheses (exact-screen-size topmost GL window vs DWM, shared texture cache) are untested.

Related: [[overview]] · [[decisions]] · [[index]]

## Virtual Desktop startup + F11 flicker (root cause found + fixed 2026-11-19; uncommitted)

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
- **Tests:** `VdDesktopTests` 14 pass (`coverRect`); new opt-in GUI `VdWindowTests` (5 pass, needs real desktop+GL, skips otherwise): F11 keeps HWND,
  GL context, 0 hide/show, exact geometry; first-frame signal once. It FAILS (6 hides) if the old `setWindowFlags` path is restored.
- **NOT verified:** the real Motiva.exe was not driven (user's instance was running; not touched); real video file wallpaper (no sample file; a
  timer source + real Binary Rain `VideoPlayer` source were used); multi-monitor / hot-plug (1 monitor); other GPUs/drivers (NVIDIA/AMD may
  differ); a visible flicker can't be seen from here - results are event logs + screen-capture coverage. Flat partial-coverage plateaus
  (0.67-0.93) seen in some harness runs, not oscillation; attributed to an external overlay, unproven.
- **Diagnostics kept:** `VdDiag` (env `MOTIVA_VD_DIAG=1`, logs `[VdDiag]` lines to Motiva.log: handle/show/hide/resize, GL context,
  swap gaps >150 ms, DISPLAYCHANGE/DWM msgs). Idle when env unset.
