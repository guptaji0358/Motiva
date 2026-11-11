# Motiva — overview

Last verified: 2026-11-11 against the working tree (branch `main`, HEAD `9c653e6`).

## What it is

**Motiva** is a native Windows desktop app (C++17 / Qt 6) that plays videos and
pictures as the live desktop wallpaper, with playlists, a web-media bridge and
an Explorer right-click menu. No Electron, no browser engine, no fake fullscreen
window. Product name is "Motiva" everywhere user-facing (exe `Motiva.exe`,
window title, tray tooltip, registry `HKCU\Software\Motiva`, log
`%TEMP%\Motiva.log`). Source folder names (`VideoWallpaper` history, class
names like `WallpaperManager`) are historical and intentionally kept.

Repo location: `D:\Apps\MyApps\Pendiing\Motiva` (under OneDrive; see
[[development-workflow]] for the build-lock caveat).

## Major capabilities (as implemented)

- **Desktop wallpaper**: video (MP4 etc. via Qt Multimedia) is attached behind
  desktop icons using the undocumented Progman/WorkerW technique, rendered
  through DirectComposition + D3D11. One window per monitor. Details:
  [[architecture]], [[data-flow]].
- **Images and wallpaper**: pictures can be set as wallpaper; GIF is picture.
- **Playlists**: image, video and *mixed* playlists stored in a SQLite library
  (`.mtv`). Items have an order, rotation triggers (unlock / Windows start /
  interval), per-playlist **categories** (condition-based or hand-picked),
  nested category tree, and a "Wallpaper Flow" node editor. See [[modules]].
- **Media recovery**: "Find File" / "Find All at Once" relocate missing media
  in one directory walk; optional **Local Media Backup** (off by default).
- **Browser extension** (`extension/`, MV3, Chrome/Edge/Brave): right-click
  web images/videos -> set wallpaper, add to playlist, or save to cache, via
  Native Messaging through `MotivaBrowserHost.exe`.
- **Explorer integration**: a single `IExplorerCommand` COM handler
  (`MotivaShell.dll`) gives "Motiva" submenu on media files and folders; also
  a legacy static verb path. See [[modules]].
- **Cloud (read-only, media-only)**: view/download pictures and videos from
  Google Drive / OneDrive / Dropbox through a bundled **rclone** binary.
  Motiva has no OAuth code of its own.
- **Cleanup & reset**: `CleanupManager` is the only code that deletes Motiva
  data (cache, data, factory reset).
- **Notifications**: unified `NotificationManager` (in-app toasts + Windows
  notifications when not foreground).
- **Theme**: Onyx and other themes plus a theme editor with per-theme overrides.

## Current maturity (honest summary)

Most UI is GPU-drawn (`GlUi`, `GlWindow`); a few dialogs and all OS pickers
remain Qt Widgets. Desktop attach works on the dev machine (Windows 11 build
26300-class) but several Windows-level behaviours were only verified with
proxies (frame counts, Win32 state) rather than by eye. See [[known-issues]].

## Related memory

- Structure of the repo: [[project-structure]]
- Tech stack and versions: [[technology-stack]]
- Why things are the way they are: [[decisions]]
- Historical dated status log: `history/status-log-*.md` (linked from [[index]])
