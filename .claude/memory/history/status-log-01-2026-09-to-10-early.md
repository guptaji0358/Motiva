# CLAUDE.md

Guidance for Claude Code (or any future agent) working in this repo.

## Current status (2026-10-25b, Explorer menu is now one COM handler)

The static registry cascade is replaced (when `MotivaShell.dll` is present; otherwise the old static
single-file menu is the fallback) by ONE `IExplorerCommand` in `scripts/shell/MotivaShell.cpp`
(CLSID `{2D6F4A18-...}`, verbs `...\shell\Motiva` on every supported extension AND `Directory` use
`ExplorerCommandHandler`). It reads everything from values on its CLSID key (`Exe`, `Icon`,
`Image/Video/OtherExtensions`, `Playlists` REG_MULTI_SZ `id|i/v/m|name`, `LastPlaylistId`), written by
`WindowsShellIntegration` (Register = extensions, `UpdateExplorerPlaylistMenu` = playlists + last id).
Menu: Set as wallpaper (exactly one file) / Add to "<last playlist>" / Add to playlist > ... / Create New
Playlist. Gets the WHOLE selection in one call (no per-file process, no 15-file cap); folders qualify only
if they directly contain an image; image+video selection lists only mixed playlists. Invoke writes the paths
to a temp file and starts `Motiva.exe --explorer-add <playlistId|0> <file>` (main.cpp reads+deletes it, paths
joined by '\n' through InstanceIpc; `onAddToPlaylistReceived` now takes that list, "0" = new playlist, mixed
selection -> Mixed playlist). Last-used playlist = `SettingsManager::explorerLastPlaylistId`, updated on
every successful add/create. Startup re-registers, so upgrading needs no user action (Explorer may need a
restart to release an old DLL). Verified: build; a COM harness on a fake IShellItemArray (state/enum/titles/
separators for single, multi, mixed, folder, empty folder, unsupported; Invoke launches with a correct temp
file). NOT verified: the real Explorer right-click, the full `--explorer-add` -> running-app path, and the
Windows 11 TOP-LEVEL menu (needs a signed sparse package with identity; the handler is the same interface).

## Current status (2026-10-25, browser extension: web media -> cache -> wallpaper/playlist)

`extension/` is a Manifest V3 extension (Chrome/Edge/Brave; fixed ID via the `key` in manifest.json,
same ID is in `BrowserIntegration::kExtensionId` and the host's origin check). Right-click an image/video
-> Motiva > Set as wallpaper / Add to playlist > <playlists> / New playlist / Save to cache, plus a hover
button (content.js, shadow DOM, toggle in the popup). Transport = Native Messaging:
`scripts/browser/MotivaBrowserHost.exe` (Qt-free relay, built by CMake, shipped beside Motiva.exe) forwards
each JSON request as WM_COPYDATA command 5 to the running Motiva's `InstanceIpc` window and relays replies
(command 6, sent to the host window passed in WPARAM) back; it starts Motiva (`--autostart`) when a request
needs it, but NOT for `playlists`/`abort` (browser startup must not launch the app). `ping` is answered by
the host. The host finds fields with a naive `"key":"` scan, so the extension must send compact JSON.
`MainWindow::onBrowserRequest` handles playlists / chunk / abort / media; web files go through
`WebMediaCache` (`<AppLocalData>/web-cache/<hash>/<name>`): the extension first fetches in the browser
(cookies work) and streams base64 chunks, falling back to Motiva downloading the URL itself (Referer =
page). blob:/data: go through the content script. Validation: http(s) only, no text/HLS, 2 GB cap,
image must be decodable. Cached files are never auto-pruned (a playlist may use them); the Cleanup
"DOWNLOADED MEDIA" row (formerly cloud downloads) deletes web-cache + cloud-cache. Settings >
Integration > "Browser extension" registers the host (`BrowserIntegration`: manifest under
AppLocalData/browser + HKCU NativeMessagingHosts for Chrome/Edge/Brave; installer uninstall removes the
keys; Deploy.ps1 ships the host and `extension/`). The toggle's refresh must use QSignalBlocker.
Verified: build; host protocol and the app end to end with a Python stand-in for Chrome (ping, not-running
playlists, URL download, chunked upload, HTML rejection, playlist list); JS syntax. NOT verified: loading
the extension in a real browser, the registry/manifest discovery by Chrome, the hover button/menus,
wallpaper and playlist actions through the bridge, HLS/DRM/streaming sources (unsupported), sites that
block hotlinking by Referer when the browser fetch fails.

## Current status (2026-10-24b, rclone.exe branded)

`scripts/tools/brand_rclone_exe.py` replaces rclone.exe's icon (RT_ICON/GROUP_ICON) and version
strings with Motiva's (resources only; language 1033 - UpdateResource needs the exact lang).
Applied to third_party/rclone, build/rclone, deployment/resources/bin/rclone; the sign-in page is
patched by `patch_rclone_page.py`. NOT changeable here: the Google/Microsoft/Dropbox consent
screen name/logo (set in your own OAuth app registration).

## Current status (2026-10-24, cloud is read-only and media-only)

Policy, enforced in `CloudManager` (not just hidden in the UI): the cloud is VIEW/DOWNLOAD
only and only pictures/videos (`CloudManager::isAllowedMediaName`, GIF counts as a picture)
are ever listed or fetched. Removed entirely: upload, new folder, sync/mirror/two-way,
library/folder backup, restore, dry-run preview/verify (CloudWindow now has two pages:
Accounts / Browse). `list()` drops non-media files (folders stay, for navigation);
`planCacheDownload`/`planDownloadTo` skip folders and non-media (no whole-folder copies).
Sign-in asks for read-only scopes: Google `drive.readonly`, OneDrive `access_scopes=Files.Read...`;
Dropbox cannot be restricted by the client - register the Dropbox app with read scopes only.
Verified: build only. NOT verified: real sign-in with the read-only scopes, listing filter
against a live account. `docs/CLOUD_STORAGE.md` still describes the removed sync/backup features.

## Current status (2026-10-23b, Cloud window on the GL UI)

`CloudWindow` is now a `GlWindow` (same public API: `setTarget/present/filesReady`;
CloudManager/RcloneRunner untouched). Four pages (Accounts / Browse / Sync /
Backup & Restore, `GlButton::Kind::Tab` row) + an always-visible transfer panel
(`GlProgressBar`, detail line, log `GlItemView`, hidden when the window is short).
Per-page items live in `m_pageItems`; `updateVisibility()` combines page + flags
(connecting, reset, target, banner, dev-only credentials button). Lists are
`GlItemView` over `QStandardItemModel`; Browse rows are check-box rows (the view is
single-selection): click/Space toggles, double-click/Enter opens a folder; path is a
text label, not clickable crumbs. Sync report = summary label + line list
(`setSyncReport` splits the old text). Dialogs use `GlMessageBox`; QFileDialog stays
native; the dev-only API-credentials dialog (`MOTIVA_CLOUD_DEV`) is still a plain
QDialog so the secret stays masked. Verified: build, and an out-of-tree harness
(static-Qt objects relinked with a test main; `canvas()->snapshot()`) grabbing all
four empty-state pages. NOT verified: populated lists, real transfers, sign-in,
other themes, small screens (window capped at 92% of the screen).

## Current status (2026-10-23, cloud storage via rclone)

Replaces the 2026-10-22 hand-written OAuth cloud layer (kept, unbuilt, in
`scripts/superseded-oauth-cloud/`; it was never committed). Motiva has NO OAuth /
token / provider-API code: **rclone** (MIT, bundled 1.75.2 in `third_party/rclone/`,
shipped in `resources/bin/rclone/` with LICENSE.txt) does sign-in, refresh, listing
and transfers. `RcloneRunner.{h,cpp}` = `RcloneJob` (one async QProcess; JSON-log
progress/errors, timeouts, Ctrl+C-then-kill cancel, `scrub()` of secrets, error
classification). `CloudManager` (singleton) = tool check (`rclone version`, >=1.65),
config encryption (rclone-encrypted `rclone.conf`; random key under DPAPI in
QSettings `cloud/rcloneKey`, passed via `RCLONE_CONFIG_PASS`), accounts (`config
create --non-interactive` answered step by step; reconnect), `lsjson` browsing,
and "plans" (list of rclone steps run one at a time: cache download for playlists,
download/upload, copy, mirror with `--backup-dir`+`--max-delete`, library backup via
`VACUUM INTO`, restore under a new name) plus dry-run preview / `check`. `CloudWindow`
= plain Qt Widgets QDialog (Accounts / Browse / Sync / Backup & Restore + transfer
panel), themed by the app style sheet; same `setTarget/present/filesReady` API the
PlaylistDialog "Cloud..." button already used. Details, safety rules and the
product-owner setup (Google needs your OWN client ID - rclone's shared one is being
retired and Motiva refuses to connect Google without it): `docs/CLOUD_STORAGE.md`.
Gotchas found by testing: `copyto`/single-file `copy` IGNORE `--immutable` (single
files are copied with an exact `--include`); Ctrl+C must be ignored in-process while
attached to rclone's console or Motiva dies; QProcess `finished` can precede the
last stderr read (drain in `finish()`); beware MSYS path mangling of `/x` args when
testing from Git Bash. Verified with an out-of-tree harness (`:local` backend, a
fake expired-token Drive remote, `:webdav` to a closed port): tool/version, config
encryption, listing, upload/download/immutability/overwrite+trash, mirror preview+run,
max-delete brake, verify, progress, cancel (no partials), library backup/restore,
scrub, reset; UI grabbed under 3 themes. NOT verified: any real sign-in/listing/
transfer against Google/OneDrive/Dropbox (no accounts/credentials here), reconnect
(`config update ... config_refresh_token=true`) against a live account, Explorer-level
use of the window by a person, rclone's third-party Go module licence notices,
Cleanup window rows for `cloud-trash` / `Motiva Sync Trash`.

## Current status (v2.1, folder context menu)

Right-clicking a FOLDER shows `Motiva > Add to playlist > <image playlists> /
Create New Playlist...` only when the folder directly contains an image
(no sub-folder search). Static verbs can't inspect contents, so
`scripts/shell/MotivaShell.cpp` builds `MotivaShell.dll` (Qt-free, per-user
COM server, `IExplorerCommandState`, ships in `resources/bin/`); the verb
`Directory\shell\Motiva` has `CommandStateHandler={7B3F0C52-...}` and the
CLSID key holds `ImageExtensions` (written by
`WindowsShellIntegration::registerFolderMenu` from the app's image list).
No DLL = no folder verb. The command reuses `--add-to-playlist-id/-new-playlist`
with the folder path; `MainWindow::onAddToPlaylistReceived` expands a
directory to its images (natural order; new playlist is named after the folder).
Verified: DLL GetState (image/no-image/image-only-in-subfolder) via a harness.
NOT verified in real Explorer: that it honours CommandStateHandler together
with ExtendedSubCommandsKey (needs the user's right-click check after
re-toggling Explorer integration in Settings). Explorer locks the DLL while
loaded, so updating it may need an Explorer restart.

## Current status (2026-10-18, GPU-drawn main window + theme editor)

**Main window is now GL-drawn** (`GlUi.{h,cpp}`): one `QOpenGLWidget` (`GlCanvas`)
paints an animated GLSL background (falls back to a QPainter gradient if the
shader fails) and light retained-mode items (`GlPanel/GlLabel/GlButton/GlPreview`)
with QPainter on the same GL context; colors come from `Theme::themePalette`
(so Theme-editor customizations apply; Onyx keeps sharp corners via
`Style::radius`). `MainWindow` is borderless (`FramelessWindowHint`);
`nativeEvent` answers WM_NCHITTEST (title strip = caption unless over a button,
7px resize edges), `showEvent` asks DWM for rounded corners. `MainWindow::buildUi`
/ `layoutGl` replaced the widget layout; backend logic unchanged. Settings and
Playlist dialogs are still Qt Widgets. The preview frame is CPU-scaled to panel
size before upload (never upload an 8K frame). Animation timer stops when the
window is hidden/minimized. NOT done: screen-reader accessibility for GL items,
reduced-motion setting, converting the remaining dialogs.
**Deployment**: build now needs Qt6OpenGL.dll + Qt6OpenGLWidgets.dll (copied
to `deployment/resources/Qt/`; check the installer/MSIX file list too).
**Theme editor**: `ThemeCustomization.cpp` (sparse per-theme overrides,
accent presets, WCAG contrast fixes), `ThemeEditorDialog`, persisted as JSON in
`themeEditor/<Theme>/colors` via `SettingsManager::themeOverrides`.

