## Current status (2026-10-08, startup robustness + SVG status icons + Motiva tooltip)

**Startup**: `MainWindow`'s constructor now builds only UI/tray/IPC/power
registration; `beginDeferredStartup()` (called from `main()` after `show()`)
runs `completeStartup()` - media restore, Windows-session playlist startup,
Explorer hand-offs, wallpaper re-apply, backup start. `InstanceIpc` now
listens as the FIRST constructor step and the senders wait (bounded, sender
process only) for the listener - previously a second launch during boot found
no listener and exited silently ("Motiva didn't open"). IPC requests that
arrive before startup finishes are queued (`runAfterStartup`). The modal "no
system tray" box in `main()` is now a log line (tray is routinely missing for
seconds after sign-in). `WallpaperManager` seeds `m_onBattery` from
`GetSystemPowerStatus` so startup restore doesn't assume AC. The preview's
video page shows "Loading preview..." until a frame exists (was an empty
#161616 panel). `RefreshDesktopBackground()` at startup runs on a worker thread.
Cold start measured 3.7 s in the UI-build phase (disk/plugin loading) vs 0.6 s warm.

**Icons**: `Theme::tintedIcon()` (cached, SVG recolored via SourceIn) for
status marks: `Assets/status/*`, `Assets/playlist/video-badge.svg`. Themed
button icons: `Assets/flow/{dark,light}/`, `Assets/category/{dark,light}/`
(IconButton + refresh in `changeEvent`).

**Tooltips**: `MotivaToolTip` (one app-level event filter + one popup widget,
installed in `main()`) replaces Qt's QTipLabel for widget, item-view and
graphics-view tooltips; colors read from the active Theme at paint time.

## Current status (2026-10-05, Explorer "Motiva >" menu + playlist categories)

**Explorer menu**: one cascading `shell\Motiva` verb per extension
(`ExtendedSubCommandsKey` → `Motiva.ExplorerMenu.{Video,Image,Background}`),
replacing the old two flat root verbs. "Add to playlist" entries live in
`Motiva.ExplorerMenu.{Image,Video}Playlists` and are rewritten by
`MainWindow::syncExplorerPlaylistMenu()` whenever the library changes; each
command carries the playlist **id** (`--add-to-playlist-id <id> "%1"`,
`--add-to-new-playlist "%1"`), forwarded over `InstanceIpc` command 4
(`"<id>\n<path>"`). GIF gets "Set as background" only (no playlist type
accepts GIFs). Verified by building the real shell context menu with a
scratch `IContextMenu` dump tool run outside the MSIX container.

**Playlist categories** (library schema 3): `categories` +
`category_playlists` (category id <-> playlist id, both FKs cascade) - an
organizational layer over existing playlists only; categories hold no media
and are never played. A playlist may be in several categories or none
("Uncategorized"). Sidebar = `PlaylistTreeModel` (tree over
`PlaylistListModel`, search via `LibraryDatabase::searchPlaylists` on
playlist name/type/category name) + `CategoryBuilderDialog` (Build Category /
Choose Playlists, real QCheckBoxes so screen-reader Toggle works). Schema 2
existed only in an unreleased build (media-based "virtual categories");
`migrateV2toV3` turns each such media group into a playlist of the same
name inside the same-named category. `open()` must `finish()` its
`quick_check` statement - an active statement makes a migration's
`DROP TABLE` fail ("database table is locked"), and a failed migration
makes `open()` set the library aside as unreadable. Shortcuts: Ctrl+Alt+N
new image playlist, Ctrl+Shift+N new video playlist; Ctrl+N unbound.
**Do not inject synthetic keyboard input to test shortcuts** - the user's
PC is in use and keystrokes land in whatever app has the foreground.
WMI-launched test processes need `Win32_ProcessStartup.ShowWindow=1`, or
Windows applies SW_HIDE to their first default-shown window.

## Current status (2026-10-05b, per-playlist categories + Local Media Backup)

**Categories (library schema 4)** replace the schema-3 playlist-groups design:
a category is a saved filter belonging to exactly ONE playlist
(`playlist_categories`: `playlist_id` FK cascade, `filter_definition` JSON,
unique `(playlist_id, name NOCASE)`). Every playlist has a built-in "All"
(not stored, not renamable/deletable). No global category list; no media is
duplicated - `LibraryDatabase::matchingItems` always filters
`WHERE playlist_id = ?`. UI: category combo + "Build Category"
(`CategoryFilterDialog`) inside `PlaylistDialog`.

**Two category modes (schema 6)**: `playlist_categories.mode` is `condition` (JSON rules) or `selected` (hand-picked, "Select from Playlist"). Selected members live in `category_items(category_id, item_id)` -> `playlist_items.id`, both FKs cascade, so removing an item from the playlist removes it from the category and no media/item rows are duplicated. `setCategoryItems` only stores items of the category's own playlist. UI: Build Category asks Conditions vs Select from Playlist; `CategoryItemPickerDialog` does create and "Manage Selection...".

**Schema 8**: a "Find Similar" category mode (schema 7, dev build only) was removed. `migrateToV8` (runs for v6 and v7 libraries) drops `media_features` and converts any leftover non-condition/non-selected category into a hand-picked one, keeping its members. Only the two modes above exist.

**Wallpaper Flow + Arrange (schema 9, `flow_layout`)**: `FlowDialog` (QGraphicsView, lightweight node/edge items, theme-aware) edits the ONE playlist order (`playlist_items.position`, what `PlaylistModel::advance()` follows) through `PlaylistModel::setOrder` (DB first, one transaction); every edit is saved immediately, undo/redo keep full-order snapshots (cleared on add/remove). A category scope only permutes the positions its items already occupy. `flow_layout(item_id PK -> playlist_items cascade, x, y)` is presentation only; dragging a node saves its position, a sequence edit needs a drop on an arrow or Earlier/Later/menu. The playlist view's Arrange None / A-Z / Z-A is a view-only `QSortFilterProxyModel` inside `PlaylistView` (cards numbered by playlist position; drag/move disabled while arranged); the Flow dialog's Arrange menu is the explicit, undoable way to write A-Z/Z-A into the saved order. `slots` is a Qt macro - don't name variables that.

**Local Media Backup (schema 5, `media_backups`)** - OFF by default
(`backup/enabled`), never copies/scans unless enabled. `BackupManager` ->
`BackupProvider` (abstract) -> `LocalBackupProvider` (objects named by
SHA-256 under `AppLocalDataLocation/backups/objects/`, `.partial` temp
then rename). One row per media record; identical content stored once.
SQLite connections are thread-bound: **all DB work stays on the GUI
thread, workers do file I/O only.** Restore from Backup lives in the
missing-media banner, never overwrites, keeps the media id
(`PlaylistLibrary::relocateMedia`). Deletion only via Cleanup window
("MEDIA BACKUPS" row, Delete Media Backups also turns Backup off; "Delete
unused backups" under More...). Factory Reset does not delete backups.
Originals are only ever read.

**Testing gotchas**: UIA hangs ~27 s while a modal dialog opened via UIA
Toggle is open; enable Backup in tests by setting
`HKCU\Software\Motiva\Motivaackup\enabled=true` via WMI `reg add` and
relaunching. Windows' combo popups don't commit via UIA.

## Current status (2026-10-04, Settings → Cleanup & Reset)

`CleanupManager` (`scripts/{include,src}/CleanupManager.*`) is the only code
that deletes Motiva data; `CleanupWindow` is its UI - a separate window opened from Settings' compact
"Open Cleanup" entry (one instance, re-focused when reopened).
Levels: Delete Cache (log truncated in place, stale recovery `.tmp`,
in-memory thumbnails - there is no on-disk thumbnail cache), Delete Data
(library closed via `PlaylistLibrary::closeForCleanup()`, `.mtv` +
`-journal` + `*.unreadable-*.mtv` removed, fresh library re-created via the
normal schema path, saved media references cleared), Cache + Data, Factory
Reset (also turns off Run key/Start Menu shortcut/Explorer verbs, clears
all QSettings, `RecoveryState::removeForFactoryReset()` suspends writes,
then restarts with `--after-reset`, which waits for the single-instance
lock). "More…" only lists operations whose data exists. Deletion is
exact-path only, inside approved roots, rmdir-only for folders.

**Testing gotcha - Claude Desktop is an MSIX package**: any process launched
from this tool session (including `build/Motiva.exe`) sees *virtualized*
`%APPDATA%`/`%LOCALAPPDATA%`/HKCU - writes land in
`%LOCALAPPDATA%\Packages\Claude_pzs8sxrjxfjjc\LocalCache\...`, and deleting
an overlay copy reveals the real file underneath. Earlier sessions' "verified"
storage results were partly against that overlay. To test against real
storage, launch outside the package via WMI
(`Invoke-CimMethod Win32_Process -MethodName Create`), and back up / inspect
real state the same way.

