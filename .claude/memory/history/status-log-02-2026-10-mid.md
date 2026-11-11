## Current status (2026-10-18c, dialogs + popups on the GL UI)

`GlWindow` (`GlWindow.{h,cpp}`) is the base for utility windows (title strip,
content panel, footer helper, borderless hit-testing, rounded corners) with
`GlStack` for top-to-bottom row layout and shared delegates (`GlTextDelegate`,
`GlCheckRowDelegate`, `GlTableDelegate`). Converted onto it: Backup, Cleanup
(+ "More"), Theme editor (+ `GlColorDialog`, contrast-fix list in `GlColor.*`),
Category filter / item picker, Find File / Find All (`MediaRecovery.cpp`) and
Wallpaper Flow (chrome only: the node diagram is still a `FlowView`
QGraphicsView child widget laid over the canvas). `GlPopups.{h,cpp}`:
`GlMenu` (QMenu-shaped popup, raster-painted, used by item/playlist/category
menus AND the tray: tray context is `activated(Context)` -> `m_trayMenu->popup`)
and `GlMessageBox` (QMessageBox / QInputDialog-shaped; `getText`, `getItem`).
Still native: QFileDialog pickers (OS dialogs), the system tray icon itself.
Recovery dialogs were built but not exercised (no missing files to test with).
Unused now but still compiled: `DropZoneWidget`, `IconButton`, `CategoryTreePicker`.

## Current status (2026-10-18b, Settings + Playlists windows on the GL UI)

`Settings` and `Playlists` are now borderless GL windows (same `GlCanvas`,
`GlUi::handleFrameHitTest/applyRoundedCorners`). GlUi gained `GlToggle`,
`GlSlider`, `GlDropdown` (popup, compact mode, per-item data), `GlTreeDropdown`
(category tree), `GlTextInput` (single-line: caret, selection, clipboard, clear
button, no IME), `GlScrollBar`, `GlItemView` (model-driven list/card grid that
paints through a `QStyledItemDelegate`, hidden rows, wheel + scrollbar,
in-canvas drag-to-reorder, external-drop insertion bar) and item hooks
(clip rect, hover/context-menu/double-click/wheel). `SettingsDialog` keeps its
logic (controls are API-compatible GL equivalents; tabs/sidebar/grouped
layouts preserved). `PlaylistView` is now a QObject adapter over `GlItemView`
(arrangement proxy, source-row mapping); the `PlaylistDialog` handlers are
unchanged except widget plumbing. GlItemView clears hidden rows on any
structural/sort change - the owner re-applies its filter (PlaylistDialog does).
Delegates must clip with `Qt::IntersectClip` (a replacing clip escapes the
view). Still Qt Widgets: Flow, Category filter/picker, media-recovery, Cleanup,
Backup, ThemeEditor dialogs, and all QMenu / QInputDialog / QMessageBox /
QFileDialog popups. No screen-reader support for GL items.

## Current status (2026-10-11b, mixed playlists + nested categories)

**Mixed playlists**: `PlaylistType::Mixed` ('mixed' was already allowed by the
schema CHECK - no migration). ONE ordered `playlist_items` sequence; each
item's kind comes from its file extension (`PlaylistModel::isVideoAt`,
`currentIsVideo`), `media.type` is stored per file. Playback reuses the
existing pipeline: a current VIDEO is sequenced (advances on end-of-media,
honouring "Loop video"), a current IMAGE advances on the image triggers
(unlock / Windows start / interval); `PlaylistRotation::fire` ignores
triggers while a video is current. Explorer "Add to playlist" lists mixed
playlists under both image and video. Creation: "Mixed Playlist" button,
Ctrl+Alt+M. Move to Start/End: item menu + Ctrl+Home/End.

**Nested categories (no schema change)**: parent links live in
`library_metadata.category_parents` (JSON child->parent), read through
`LibraryDatabase::loadCategoryParents` which drops entries for missing
categories / other playlists / cycles; `setCategoryParent` rejects self,
cross-playlist and cycles; deleting a category promotes its children;
deleting a playlist prunes (ids can be reused). `PlaylistFilterInfo::parentId`.
Names stay unique per playlist (DB unique index). "Include subcategories"
(`include_subcategories` meta, default on) is resolved in ONE place,
`PlaylistLibrary::categoryItemIds`, so the view and wallpaper scope agree.
UI: `CategoryTreePicker` (tree popup with expand/collapse) replaces the
category combo; the "Category:" text label is the folder icon
`Assets/category/category-folder.svg`. Item context menu "Add to Category"
(hierarchical; only hand-picked categories accept items - rule-based are
shown disabled; "New Category..." at the bottom) via
`PlaylistLibrary::addItemsToCategory`. The playlist card view is single
selection, so assignment acts on the one selected item (API takes a set).
Verified with out-of-tree QtTest-style harnesses (library + model + dialog
grabs); a future `parent_id` column would be the clean upgrade path.

## Current status (2026-10-11, tabbed Settings + playlist-add icon)

`SettingsDialog` groups its (unchanged) controls into pages - General,
Appearance, Display and Playback, Notifications, Integration - created once
and only re-parented by `applyNavigationLayout()` (0 Tabs, 1 Sidebar, 2
Grouped page; new styles = new case + combo entry). The "Settings layout"
combo applies live and persists as `app/settingsLayout`
(`SettingsManager::settingsLayout()`, cleared by resetPreferences). Tab /
`QListWidget#settingsNav` / `QScrollArea#settingsPage` rules live in both
Theme.cpp stylesheets (Onyx has no %6/%7 args - use %15 for hover there).
The two New Playlist buttons use `Assets/playlist/playlist-add.svg`
(`:/playlist/playlist-add.svg`, theme-independent blue badge, derived from
the former `Assets/create_button.svg`). Verified with an out-of-tree
offscreen-style harness (grab() of all 4 themes x 3 layouts).

## Current status (2026-10-10b, unified notifications)

`NotificationManager` (singleton, `NotificationManager.{h,cpp}`) is the ONE
place feedback is routed: callers fill a `Notification` (kind, category,
title, message, key, `desktop`, `rateLimit`) and call `notify()`; it applies
the `notifications/*` settings live (SettingsManager `notify*`, all default
on), de-duplicates (same key+text inside a 30 s / 1.5 s cooldown, `rateLimit`
for "any repeat"), flood-guards routine messages (8 per 10 s), then shows an
in-app toast (`ToastHost` per window: MainWindow + PlaylistDialog,
top-right under the header, max 3 visible, click to dismiss, hover pauses,
hand-painted from the active Theme) and/or a Windows notification via
`QSystemTrayIcon::showMessage` - desktop only for `desktop`-flagged events
and only while Motiva isn't the foreground app. notify() is thread-safe
(queued to the GUI thread). `notify()` returns whether a toast was shown;
`PlaylistDialog::announce()` falls back to its inline notice line only when
not. Sources: wallpaper added/created/deleted/missing/library write errors
(PlaylistDialog), playback active/changed/failed/missing (MainWindow), Find
File / Find All outcomes. Wallpaper-error and library-error QMessageBoxes
were replaced by toasts; all confirmation dialogs are unchanged. No test
framework exists in the repo - verified with an out-of-tree offscreen QTest-
style harness. Native toast display itself cannot be verified headless.

## Current status (2026-10-10, Find All at Once - single-pass media recovery)

**Root cause fixed**: "Find File" ran one `MediaRecoveryDialog` -> one
`MediaRecovery::search()` per missing item, each a full phased walk (nearest
folder -> media folders -> every drive) with its own visited set, so N
missing files = N traversals of the same disks. `MediaRecovery::searchMany()`
(`MediaRecovery.{h,cpp}`) now matches ALL requests against every directory
entry in ONE walk (`MultiSearcher`: case-folded name hash + suffix hash for
renamed copies; one visited set; phase roots are the de-duplicated union).
`search()` is a one-request wrapper, so single Find File uses the same code.
A path-only, disk-revalidated cache (`g_foundCache`) lets a repeat request
skip the walk. Unreadable folders (access denied / drive gone) are counted
via a probe that only runs on empty listings. UI: `BulkMediaRecoveryDialog`,
opened by "Find All at Once" in the missing-media banner (shown when 2+ files
are missing); results per file: Found / Choose a match / Not found / Couldn't
be searched. Cancel keeps partial matches. Library is only touched by
`PlaylistDialog::onFindAll()` via `relocateMedia` (no schema change).
Measured on a 12k-file/312-folder tree, 20 files: 6240 -> 312 directory
listings, ~0.75 s -> ~0.07 s CPU.

