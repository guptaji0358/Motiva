# Modules — responsibilities

Class names are as found in `scripts/include/*.h` (Qt-prefixed `Q*` omitted).
Read the header first; this file only says what each piece owns.

## Entry, lifecycle, IPC

- **main.cpp** — logging (`%TEMP%\Motiva.log`, seek-to-end before each write),
  single-instance `QSharedMemory`, CLI flags: `--autostart`, `--after-reset`,
  `--set-background`, `--add-to-playlist-id <id|0> <file>`, `--add-to-new-playlist`,
  `--explorer-add <playlistId|0> <file>` (paths read from a temp file, joined by `\n`).
- **MainWindow** — borderless GL main window: header, preview, video info,
  primary Set/Remove button, status line, tray menu. Owns `WallpaperUiState`
  (NoVideo / Ready / Applying / Active / Error). Handles drag & drop of MP4/URL
  and browser requests (`onBrowserRequest`). Does **not** auto-reattach on cold
  start (deliberate, see [[decisions]]).
- **InstanceIpc** — message-only window, `WM_COPYDATA`; commands 4 (explorer
  add), 5 (browser request), 6 (reply to browser host); queues requests until
  startup completes.
- **StartupDiagnostics** (header-only singleton) — named checkpoints with ms
  stamps; logged each launch; persisted via `RecoveryState`.
- **RecoveryState** — JSON at AppData, atomic temp-file + `MoveFileExW`; holds
  `cleanExit`, `wallpaperWasAttached`, startup timings. Never authoritative for
  live attach state.

## Wallpaper / desktop

- **WallpaperManager** — one `WallpaperWindow` per monitor; attach on a worker
  (`QtConcurrent`), 300 ms bounded retry while desktop hierarchy is not ready,
  700 ms post-attach verification via `presentedFrameCount`; generation counter
  `m_attachGeneration` discards stale jobs; `wallpaperVerified` / `wallpaperActivated`
  signals; detects external wallpaper change (`WM_SETTINGCHANGE`) and steps aside.
- **WindowsDesktopWallpaper** — the only raw Win32 desktop code: Progman/WorkerW
  lookup, `0x052C` spawn message (throttled to once per generation), SetParent,
  z-order behind `SHELLDLL_DefView`, `RefreshDesktopBackground` (re-apply the
  same wallpaper path as a repaint nudge), `DumpFullHierarchy` diagnostics.
- **WallpaperWindow** — plain Win32 window (own WndProc), owns a
  `D3DWallpaperRenderer`; emits `readyForReattach` (no reattach from here).
- **D3DWallpaperRenderer** — DComp target + D3D11 swap chain; `presentedFrameCount`
  atomic; `recommitAfterReparent()`; `hasValidDCompState/hasValidDevice`.
- **VideoPlayer** — QMediaPlayer/QVideoSink, shared decode, `durationMs()`,
  `loadFile()` accepts local paths or http(s) URLs; `isUsableVideoSource()` checks.
- **SettingsManager** — QSettings wrapper (`app/*`, `backup/*`, `cloud/*`,
  `notifications/*`, `theme/accent` (legacy `themeEditor/<Theme>/colors` migrated+removed), `explorerLastPlaylistId`,
  `settingsLayout`, `wasWallpaperActive`).
- **SettingsDialog** — pages: General, Appearance, Display and Playback,
  Notifications, Integration; layout styles 0 Tabs / 1 Sidebar / 2 Grouped.

## Playlists & library

- **LibraryDatabase** — SQLite `.mtv`, schema migrations (current schema **9**;
  `migrateToV8`, `migrateV2toV3` etc.), `matchingItems` always filters by
  `playlist_id`, `loadCategoryParents`, `searchPlaylists`.
- **PlaylistLibrary** — façade over the DB used by UI and wallpaper scope;
  `categoryItemIds` resolves include-subcategories in ONE place; `relocateMedia`,
  `addItemsToCategory`, `closeForCleanup`.
- **PlaylistModel** — ordered `playlist_items` sequence; `advance()`, `setOrder`
  (DB first, one transaction); `isVideoAt` decides mixed-playlist item kind.
- **PlaylistRotation** — triggers (unlock, Windows start, interval); ignores
  image triggers while a video item is current.
- **PlaylistDialog** (+ `PlaylistView` adapter over `GlItemView`) — playlist
  editor, categories combo, Build Category, Find File / Find All.
- **CategoryFilterDialog**, **CategoryItemPickerDialog** — condition-based vs
  selected categories (`category_items` membership, cascade on item removal).
- **FlowDialog** — Wallpaper Flow editor (QGraphicsView; positions in
  `flow_layout`, order in `playlist_items.position`); chrome GL, canvas Qt.
- **MediaRecovery** / **BulkMediaRecoveryDialog** — `searchMany()` single walk,
  path-based cache `g_foundCache`, unreadable-folder counting.
- **MediaPreview** — preview frame, CPU-scaled before upload.
- **WebMediaCache** — downloads/caches web media under `web-cache/`, validates
  (http(s) only, no text/HLS, 2 GB cap, images must decode).

## Backup, cleanup, cloud

- **BackupManager** → **BackupProvider** (abstract) → **LocalBackupProvider**
  (SHA-256 objects, `.partial` then rename). Off by default. Worker threads do
  file I/O only; DB work on GUI thread. **BackupWindow** is its UI.
- **CleanupManager** — only code that deletes Motiva data (exact paths inside
  approved roots, rmdir-only for folders). **CleanupWindow** is its UI.
- **CloudManager** — singleton: tool check, encrypted rclone config, accounts,
  `lsjson` browsing, "plans" (ordered rclone steps), media-only filter
  (`isAllowedMediaName`, GIF = picture). Enforces read-only policy.
- **RcloneRunner** — `RcloneJob`: one async QProcess, JSON-log progress, timeouts,
  Ctrl+C-then-kill cancel, `scrub()` of secrets, error classification.
- **CloudWindow** — GL window: Accounts / Browse / Sync / Backup & Restore pages
  (pages 3-4 are legacy from the removed feature set, see [[known-issues]]).

## Shell & browser integration

- **WindowsShellIntegration** — registers extensions, the CLSID values
  (`Exe`, `Icon`, `Image/Video/OtherExtensions`, `Playlists` REG_MULTI_SZ
  `id|i/v/m|name`, `LastPlaylistId`) read by the shell DLL; startup re-registers.
- **MotivaShell.cpp** (`scripts/shell/`) — `IExplorerCommand` handler: whole-
  selection state, submenu entries (Set as wallpaper / Add to last playlist /
  Add to playlist > ... / Create New Playlist), folders only if they directly
  contain an image; Invoke writes paths to a temp file and starts
  `Motiva.exe --explorer-add`.
- **BrowserIntegration** — writes native-messaging manifest under AppLocalData
  and HKCU `NativeMessagingHosts` (Chrome/Edge/Brave); uninstall removes keys.
- **MotivaBrowserHost.cpp** — relays JSON requests as WM_COPYDATA command 5 and
  replies as command 6; starts Motiva with `--autostart` only for requests that
  need it (not `playlists`/`abort`); answers `ping` itself.

## UI toolkit (GPU-drawn, `GlUi` family)

- **GlUi** — `GlCanvas` (one `QOpenGLWidget`, animated GLSL background), items
  `GlPanel/GlLabel/GlButton/GlToggle/GlSlider/GlDropdown/GlTreeDropdown/
  GlTextInput/GlScrollBar/GlPreview/GlProgressBar/GlItemView`; `GlItemView` is
  model-driven and paints via `QStyledItemDelegate`.
- **GlWindow** — borderless utility-window base, title strip, `GlStack` layout,
  shared delegates (`GlTextDelegate`, `GlCheckRowDelegate`, `GlTableDelegate`).
- **GlPopups** — `GlMenu` (QMenu-shaped) and `GlMessageBox` (`getText`, `getItem`).
- **GlColor** — `GlColorDialog`, `GlSwatchButton`.
- **Theme / ThemeCustomization** — palette tokens; ONE global accent
  (`Theme::setAccent` sets an accent override on all 4 themes; hover/pressed/
  soft/text-on-accent derived, accent nudged to >=3:1 vs window bg); 12 presets;
  `ThemeEditorDialog` = "Accent Color" chip picker (+custom, live preview,
  Apply/Cancel; built, not runtime-verified); `Theme::tintedIcon` (cached).
- **NotificationManager** — single routing point for toasts (`ToastHost`) and
  desktop notifications; de-duplication and flood guard.
- **MotivaToolTip** — one app-level tooltip popup replacing Qt's tip label.

Related: [[architecture]] · [[data-flow]] · [[project-structure]]
