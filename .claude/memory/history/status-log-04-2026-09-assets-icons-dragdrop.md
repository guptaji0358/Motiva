## Assets folder layout (2026-09-12e, reorganized by feature)

`Assets/` is organized by feature/component, NOT by generic file-type
folders (no `icons/`/`images/`/`gifs/` - that was the previous, now
explicitly rejected, structure):
- `Assets/application/` — app identity: `motiva.ico` (+ per-size source
  PNGs) is the exe/window/tray icon; `motiva_logo.png` is a 256px
  branding copy. Anything that represents the app itself, not a specific
  feature, belongs here.
- `Assets/settings-icon/` — `settings.ico` (+ per-size source PNGs), the
  gear icon used by the header Settings button, tray menu entry, and
  `SettingsDialog`'s window icon.

No other feature (wallpaper controls, play/pause, status indicator) has
an actual asset FILE as of this reorganization - play/pause use plain
Unicode glyphs ("▶"/"⏸") and the status indicator uses a CSS-colored "●"
character, both directly in `MainWindow.cpp`, not image files. Do not
create empty placeholder folders (e.g. `Assets/wallpaper/`,
`Assets/video/`) for those until a real asset file exists for them -
this repo's own history briefly had exactly that anti-pattern (an empty
`Assets/gifs/.gitkeep`) and it was removed rather than migrated.

**Qt resource aliases match the folder layout**: `resources/app.qrc`
exposes these as `:/application/motiva.ico` and
`:/settings-icon/settings.ico` (previously `:/icons/...` - renamed
alongside the folder move for consistency, not left mismatched). Referenced
from `scripts/main/main.cpp`, `scripts/src/MainWindow.cpp`
(`kAppIconResourcePath`/`kSettingsIconResourcePath`), and
`scripts/src/SettingsDialog.cpp`. `resources/app.rc`'s `ICON` resource
(the .exe's own Explorer/taskbar icon) points directly at
`../Assets/application/motiva.ico` on disk, independent of the `.qrc`
embedding.

**When adding a new feature that needs its own asset**: create
`Assets/<feature-name>/` (lowercase, hyphenated, matching `settings-icon`'s
style) only once a real file exists for it, add it to `resources/app.qrc`
with an alias mirroring the folder path (e.g. `<feature-name>/foo.svg`),
and reference it via that same `:/<feature-name>/foo.svg` path in code -
never a raw filesystem path (keeps icon loading independent of the
process's working directory).

## Current status (2026-09-12d, Settings icon + MP4/URL drag & drop)

**Settings icon**: `Assets/icons/settings.ico` (+ per-size source PNGs),
a simple gray gear glyph on a transparent background, same neutral tone
family as the status-text colors in `MainWindow.cpp`. Embedded via
`resources/app.qrc` as `:/icons/settings.ico`, used by the header
Settings button, the tray menu's "Settings" entry, and `SettingsDialog`'s
own window icon - replaces the previous unicode "⚙" glyph. Verified
genuinely loaded (not null, not a stale duplicate) by compiling a tiny
standalone test against the real generated `qrc_app.cpp` resource blob:
`settings.ico isNull=false availableSizes=5`.

**Drag & drop**: implemented on `MainWindow` (`dragEnterEvent`/
`dragMoveEvent`/`dragLeaveEvent`/`dropEvent`), feeding into a new shared
`loadVideoSource()` - refactored out of `onChooseVideo()`'s body, so
drag & drop and the Open Video button share one load path (no second
video-loading pipeline). `extractDroppedVideoSource()` inspects
`QMimeData`: `hasUrls()` covers both Explorer file drops (`file://`) and
most browser link/video drags in one check; a local candidate must exist
on disk and end in `.mp4` (matching the Open Video dialog's own existing
filter - not widened without a reason to); a web candidate must be a
valid `http`/`https` URL whose path (query string excluded) ends in
`.mp4`. A `hasText()` fallback exists ONLY for a strictly-parsed absolute
http(s) URL - arbitrary dropped text is never interpreted as a local
path (the exact case the task's security section called out). No
playlist/queue exists anywhere in this app (`WallpaperManager` holds
exactly one current video - confirmed by inspection before writing any
of this), so multiple dropped files keep only the first valid one; extra
candidates are reported via `m_statusLabel`'s tooltip rather than
guessed at or silently merged.

**Web video playback reuses the existing pipeline, not a new one**:
`VideoPlayer::loadFile()` now recognizes an http(s) URL passed in as the
"path" string and passes it straight to `QMediaPlayer::setSource()` as a
network `QUrl` instead of `QUrl::fromLocalFile()` - `QMediaPlayer`
(FFmpeg-backed on this project) already natively streams from a network
source, so no networking/download code was added anywhere.
`isUsableVideoSource()` (new, used by startup restore, IPC recovery, and
`onSetWallpaper`'s existence check - replacing their old bare
`QFileInfo::exists()` calls) accepts either an existing local file or a
recognized web video URL, so a URL-sourced video can be set as the
actual wallpaper through the same `WallpaperManager::setWallpaper()`
path a local file uses - not a parallel one.

**Verified this session**: QUrl/QMimeData mechanics the drag&drop logic
depends on were checked directly against a standalone compiled test
(not just read/assumed) - confirmed `file://` URLs round-trip to the
right Windows path with `isLocalFile()`/`toLocalFile()`, a query-string
URL's `.path()` correctly excludes the query for extension matching, a
non-video webpage URL's path has no matching suffix (correctly
rejected), and arbitrary non-URL text fails `QUrl::fromUserInput`
validity (correctly rejected). Full rebuild succeeded; existing behavior
re-verified unaffected - normal startup (no unwanted auto-attach),
`RefreshDesktopBackground` nudge, and Set-as-Wallpaper reaching verified
`COMPLETE`/`Active` all still work exactly as before, all via the same
second-instance-IPC-recovery trick used to verify prior sessions' fixes.

**Not verified this session** (no way to perform an actual OS-level drag
gesture, drag from a real Explorer window, or drag a link out of a real
browser from this tool session - same limitation noted throughout this
file's history): an actual end-to-end drag-and-drop of a real file or
browser link onto the running window. The MIME-handling logic itself was
verified against real Qt behavior as described above, but a live
click/drag-through pass on the actual machine is still the real test
before calling this feature done.

## Current status (2026-09-12c, Assets folder + real app icon)

`Assets/{icons,images,gifs}/` created at the project root (updates the
"Assets folder" entry below from "created empty" to populated). A full
inspection first confirmed there were genuinely zero existing visual
assets anywhere (no `.png`/`.jpg`/`.gif`/`.svg`/`.ico`, no `.qrc`, nothing
embedded in source) - the only prior icon usage was
`style()->standardIcon(QStyle::SP_MediaPlay)`, an OS fallback.

**Real app icon created and wired in** (closing that long-standing
"Planned work" gap): a simple rounded-square icon (dark surface + the same
green accent used for the "Active" status text) generated programmatically
via a PowerShell/System.Drawing script at 16/24/32/48/64/128/256px, packed
into `Assets/icons/motiva.ico` (a hand-built multi-resolution ICO
container - `.NET`'s `Icon` class confirmed it loads correctly), plus
`Assets/images/motiva_logo.png` (256px copy, for future branding use).

Wired in two ways:
- `resources/app.rc` — `1 ICON "../Assets/icons/motiva.ico"`: this is what
  gives `Motiva.exe` itself a real icon in Explorer/taskbar/Alt-Tab
  (verified: extracted the running exe's associated icon via
  `System.Drawing.Icon.ExtractAssociatedIcon` and visually confirmed it's
  the new icon, not a blank/default one).
- `resources/app.qrc` (new) embeds the same `.ico` into the binary via
  Qt's resource system as `:/icons/motiva.ico` - loaded at runtime by
  `MainWindow::MainWindow` (`setWindowIcon`), `MainWindow::buildTray`
  (replaces the `QStyle::SP_MediaPlay` fallback), and `main()`
  (`QApplication::setWindowIcon`, so any icon-less dialog still shows it).
  Chosen over a relative filesystem path specifically so the icon still
  loads regardless of the process's working directory.

**A real, unrelated environment issue hit and diagnosed while rebuilding**:
after adding the `ICON`/`.qrc` resource lines, `windres`'s RC compile
started failing with an opaque `preprocessing failed` (its internal `gcc
-E` preprocessing step, `cc1.exe`, produced zero output / exited 1 with
no stderr). Bisected down to: reproducible even on the ORIGINAL,
unmodified `app.rc` when invoked with the build directory as the working
directory (which is how CMake/Ninja always invokes it) - i.e. unrelated
to this session's actual content changes. Root cause: `rm -rf build`
itself failed with `Device or resource busy` at the same time, and
`OneDrive.exe`/`OneDrive.Sync.Service.exe` were confirmed running and
actively syncing this project folder - this whole repo lives under
`OneDrive\Desktop\...`, and OneDrive's cloud-sync filter driver
intermittently locking newly-created build files is a well-known cause of
exactly this kind of silent child-process failure. Resolved itself once
the lock cleared (a `rm -rf build` + fresh `cmake -S/-B` succeeded
immediately after). **Worth knowing for future sessions**: if a build
fails with an unexplained silent tool crash (especially RC/windres, or
any step that creates many small files quickly) and nothing about the
source changed, suspect OneDrive sync contention on `build/` before
assuming a real code/config bug - retry, or `rm -rf build` and
reconfigure, before spending time bisecting source changes.

**Verified this session**: clean rebuild after the OneDrive contention
cleared (all 17 build steps succeeded, RC object included); app launches
normally, log shows the same startup sequence as before (no attach,
`RefreshDesktopBackground` nudge fires) - the Assets/icon work didn't
regress any prior lifecycle fix; the built `Motiva.exe`'s own Explorer
icon was extracted and visually confirmed to be the new icon.

**Not verified this session**: the in-app window titlebar icon and tray
icon visually (`setWindowIcon`/`QSystemTrayIcon::setIcon` calls are
present and use the same verified-loadable `.ico` resource, but this tool
session cannot see the live interactive desktop to confirm the rendered
result - same limitation noted throughout this file's history).

