# Technical decisions and rationale

Each entry: decision, why, and where the reasoning is recorded. Citations use the
archive files in `history/` (numbered status logs, see [[index]]). Do not reverse
one of these without re-reading the cited reason.

## Desktop attach

- **Progman/WorkerW + SetParent, not a topmost fake fullscreen window.** Product
  rule ("no fake fullscreen"). The window is a child of Progman behind
  `SHELLDLL_DefView`, attached once, with no periodic z-order upkeep (periodic
  reattach caused flicker). [history/status-log-10]
- **DirectComposition + D3D11 instead of GDI.** Progman carries
  `WS_EX_NOREDIRECTIONBITMAP`, so GDI children never rendered. [status-log-10]
- **Attach runs off the GUI thread; the GUI only receives a bool.** Discovery took
  6-7 s synchronously and froze startup. [status-log-09]
- **No 0x052C spawn-message loop on the hot path.** It was sent about 3x/s and
  corrupted Explorer's shell hierarchy. Now throttled to one attempt per
  generation; if no real WorkerW exists, Progman is used immediately.
  [status-log-06, status-log-07]
- **Single `AttachToDesktop` call site plus a mutex.** Two threads raced on the
  same module globals. [status-log-09]
- **Generation tokens** (`m_attachGeneration`) discard stale async attach results
  after an Explorer restart. [status-log-09]
- **Verify by frame advance, not by "SetParent succeeded".** Only an increase of
  `presentedFrameCount` counts as Active. [status-log-09]

## Startup and lifecycle

- **No auto-reattach on cold process start.** The old behaviour silently took
  over the wallpaper on every login and blocked Explorer's own wallpaper change.
  An explicit click is required; restarts inside the same process still recover.
  [status-log-05]
- **Deferred startup** (`beginDeferredStartup`): the window appears first and IPC
  is answered after startup. [status-log-03]
- **A second instance means a recovery request**, not a silent exit, because
  `TaskbarCreated` delivery proved unreliable. [status-log-08]

## Rendering and UI

- **GPU-drawn main UI and utility windows (`GlUi`/`GlWindow`).** Replaced widget
  layouts for a custom look. Qt Widgets remain where OS pickers or QMenu /
  QMessageBox semantics are needed. [status-log-01, status-log-02]
- **The wallpaper window is a raw Win32 HWND**, because a reparented QWidget's
  paintEvent never fires after SetParent. [architecture]
- **Preview frames are CPU-scaled before upload**; an 8K frame is never uploaded.
  [status-log-01 "preview"]

## Data and persistence

- **Settings in QSettings (HKCU); library in SQLite `.mtv`; recovery state in a
  separate diagnostic JSON.** [architecture]
- **SQLite connections are thread-bound; all DB work runs on the GUI thread.**
  Workers do file I/O only. [status-log-03]
- **Categories hold no media.** They filter one playlist (condition or hand-picked
  membership with cascade); no media rows are duplicated. [status-log-03]
- **Mixed playlists reuse the video and image pipeline**; the item kind comes from
  its extension. [status-log-02]
- **Local backups are OFF by default and never scan unless enabled**; originals
  are only read. [status-log-03]
- **Log writes seek to EOF first**, because two processes share one file. [status-log-08]

## Cloud

- **Use bundled rclone; no OAuth code in Motiva.** Sign-in, refresh, listing and
  transfers all run in rclone. [status-log-01 "cloud storage via rclone"]
- **Cloud is read-only and media-only, enforced in `CloudManager`**, not only in
  the UI: no upload, sync, backup or restore. [status-log-01 "read-only"]
- **Google needs the user's own OAuth client ID**; Motiva refuses the shared one.
  See `docs/CLOUD_STORAGE.md`, which is partly stale.

## Browser and Explorer

- **Native Messaging through a Qt-free relay host** that forwards to the running
  app over WM_COPYDATA. Fixed extension ID via the manifest `key`. [status-log-01 "browser extension"]
- **Compact JSON only** from the extension, because the host scans for `"key":"`.
- **Explorer uses one `IExplorerCommand` handler** that gets the whole selection in
  one call. This replaces per-file processes and the 15-file cap. [status-log-01]

Related: [[architecture]] · [[known-issues]] · [[data-flow]]
