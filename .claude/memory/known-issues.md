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

Related: [[overview]] · [[decisions]] · [[index]]
