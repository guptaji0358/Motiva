## Current status (2026-09-11, "Set as wallpaper" interference fix)

**Bug**: while this app's wallpaper mode was active (whether by a manual
click this session or auto-restored on startup via `wasWallpaperActive`),
Windows Explorer's own unrelated "Set as desktop background" (right-click
an image → Set as desktop background) could stop working correctly.

**Root cause**: `WindowsDesktopWallpaper::FindOrCreateWorkerW()` sends an
undocumented `0x052C` message to Explorer's `Progman` window, asking it to
spawn/recreate the WorkerW that hosts the desktop icon layer, whenever no
standalone WorkerW is already found (the common case on machines where
Explorer never creates one - see the entries below). This function is
re-entered on every one of `WallpaperManager`'s 300ms attach-retry-timer
ticks for as long as attach hasn't been verified - which, on such a
machine, can be indefinitely. With no throttling, that meant the spawn
message was being resent to Explorer roughly 3x/second, unbounded, for as
long as wallpaper mode stayed active - a continuous, unthrottled mutation
of Explorer's own desktop shell hierarchy, not merely our own rendering.
That churn is what corrupted Explorer's own Progman/WorkerW bookkeeping
badly enough to break its unrelated wallpaper-apply codepath, which
depends on that same hierarchy. Confirmed structurally: this function's
own header comment (`WindowsDesktopWallpaper.h`) already documented a
"negative-discovery cooldown...keyed off this [generation parameter]"
that had regressed to `Q_UNUSED(generation)` in the .cpp during the
2026-09-09 startup-delay fix, then the spawn message itself was
reintroduced in the following "wallpaper visibility investigation" session
without restoring that guard.

**Fix**: `WindowsDesktopWallpaper.cpp` now throttles the spawn attempt to
at most once per attach generation (`s_hasAttemptedSpawnForGeneration`/
`s_lastSpawnAttemptGeneration`, module-level state alongside the existing
`g_attached*` globals) - a fresh generation (new "Set as Wallpaper" click,
teardown/reconfigure, or Explorer restart, all of which already bump
`WallpaperManager::m_attachGeneration`) always gets its own one-shot
attempt; repeated retry-timer ticks within the same still-failing
generation now skip straight to the Progman fallback instead of resending
the message. No SPI_SETDESKWALLPAPER/registry wallpaper call exists
anywhere in this codebase (re-confirmed by grep) - the app never overrides
Explorer's actual wallpaper setting, it was Explorer's own shell state
being disturbed.

**Left unchanged**: the retry timer itself (still a legitimate bounded
"wait for desktop hierarchy" loop, unrelated to this bug), all
SetParent/SetWindowPos reattachment logic, DirectComposition/D3D11
rendering, the Progman-fallback z-order logic, Explorer-restart recovery,
and everything else in `WindowsDesktopWallpaper.cpp`/`WallpaperManager.cpp`
not directly involved in the spawn-message call site.

**Verified this session**: clean rebuild; confirmed via
`%TEMP%\Motiva.log` that with wallpaper mode never activated (no
persisted intent, no click), zero `[Discover]`/`[Shell]` log lines appear
at all - no Progman/WorkerW interaction happens while merely running,
which was already true architecturally and remains true. **Not verified
this session** (no way to drive Explorer's right-click "Set as desktop
background" or click through this app's own UI from this tool session):
the actual before/after interference with a `.jpg`/`.png` Set-as-wallpaper
while this app's wallpaper mode is active and stuck retrying - the fix
directly addresses the confirmed unthrottled-message defect, but a live
repro/confirm pass on the reporting user's machine is the real test.

## Product name

**Motiva** — the finished/shipping product name (executable `Motiva.exe`,
window title, tray tooltip, CMake project/target, registry/settings
storage under `HKCU\Software\Motiva`, log file `%TEMP%\Motiva.log`). The
source tree folder (`VideoWallpaper/`) and internal C++ class names
(`WallpaperManager`, `WallpaperWindow`, etc.) were deliberately left
unchanged during the rebrand - only the build target/executable and
product-facing identity were renamed. Use "Motiva" in all new
documentation, session notes, and user-facing text going forward.

## What this is

A native Windows C++/Qt6 app (`src/`) that plays an MP4 as a live desktop
wallpaper (behind icons, above the OS wallpaper backdrop) using the
undocumented `Progman`/`WorkerW` attach technique. No Electron, no fake
fullscreen overlay.

## Planned work

**Motiva UI Redesign** (previously tracked as "VideoWallpaper UI
Redesign" before the 2026-09-11 product rename above) — a first pass
landed 2026-09-11, see the status entry below. Use this exact name
consistently in session notes, handoffs, and any future documentation
that refers to this effort, so it stays a single identifiable thread
across sessions rather than getting renamed each time. Remaining/possible
follow-ups: an actual app icon (none exists yet - the tray still uses a
Qt standard icon), and this session's own "not verified" items below
(interactive click-through, dark/light Windows theme, resize behavior).

## Current status (2026-09-11, Motiva UI Redesign - first pass)

Redesigned `MainWindow`'s layout and introduced an explicit UI-only state
machine (`MainWindow::WallpaperUiState`: NoVideo / Ready / Applying /
Active / Error) so the status text and the one primary button always
reflect **verified** wallpaper state, not merely "a video loaded" or
"attach requested" - see the functional requirement in the redesign
task. This required one small, additive backend change: `WallpaperManager`
gained a `wallpaperVerified()` signal, emitted from the *existing*
post-attach `presentedFrameCount` verification checkpoint in
`onAttachAttemptFinished()` (the same check that already decided
COMPLETE/STALLED) - no attach/recovery/rendering logic was touched, only
a signal emission added at an already-existing checkpoint. The existing
`wallpaperActivated()` signal (fired the instant `setWallpaper()` is
called, well before attach is confirmed) now drives the UI's "Applying…"
state instead of "Active" - previously the status label read "Wallpaper
active." immediately on click, which was itself a latent instance of the
exact bug this task's functional rule warns against.

**Layout**: header (app name + a "⚙ Settings" button) → video preview
(now the dominant, expanding element via a stretch factor, was previously
a fixed `minimumHeight` box competing with a long settings form below
it) → a compact video-info row (filename, plus resolution/duration/format
on a second line, populated from two small new read-only getters -
`VideoPlayer::durationMs()` - and the existing `videoNativeSize()`) with
Play/Pause and Open Video as secondary buttons → a status line → one
primary button that is *either* "Set as Wallpaper" or "Remove Wallpaper"
depending on state (never both shown as competing equals, per the
redesign task's explicit instruction) with a distinct disabled "Applying…"
state in between so it can't be double-clicked mid-attach.

**Settings extracted**: scaling, monitor selection, volume, mute, loop,
and start-with-Windows moved out of the main window into a new
`SettingsDialog` (`src/SettingsDialog.{h,cpp}`), reachable via the header
button and the tray menu. It owns its own wiring to `WallpaperManager`/
`SettingsManager` directly; `MainWindow` only opens it and mirrors mute
state back to the tray's checkable "Mute" action via a small
`mutedChanged` signal - no functionality was removed, only relocated.

**Not touched, per the redesign task's explicit protection list**:
`D3DWallpaperRenderer`, `WindowsDesktopWallpaper` (Progman/WorkerW attach,
z-order, discovery), `WallpaperManager`'s attach/verify/Explorer-restart/
generation logic (only the one additive signal above), `RecoveryState`,
`StartupDiagnostics`, `InstanceIpc`, single-instance locking, and the
2026-09-11 lifecycle fixes above (startup auto-apply gating, Exit state
clearing) - all read and reused as-is, not reimplemented.

**No pre-existing theme system was found** (no `.qss`, no theme class, no
light/dark toggle anywhere in the repo before this session) - there was
nothing to preserve. The redesign leaves the app following the OS window
palette by default and only fixes a small number of colors intentionally
(status-text accent colors for Ready/Applying/Active/Error, and the video
preview's dark surface, which is a deliberate, fixed-dark convention like
other media-preview apps use regardless of app theme, not a theme break).
These are called out with a comment in `MainWindow.cpp` as the one place
that would need updating if a real theme system is added later.

**Verified this session**: clean Release build (`cmake --build build`,
zero errors); `windeployqt` bundled runtime; the app launches and runs
stably under its own log (`%TEMP%\Motiva.log` shows normal startup,
process stays alive, no crash) with the new layout, `SettingsDialog`, and
`WallpaperManager::wallpaperVerified` wiring all constructed successfully.

**Not verified this session** (no way to drive a native Win32 window's
mouse/keyboard from this tool session - the same limitation noted
throughout this file's history, e.g. the 2026-09-07 entry): actually
clicking through Open Video → Set as Wallpaper → Active → Remove
Wallpaper end-to-end; visual confirmation of spacing/alignment/typography
against the "polished, modern" bar the task set; window resize behavior
at multiple sizes; light vs. dark Windows theme rendering; Explorer-restart
recovery still driving the new `wallpaperVerified`-based "Active" status
correctly in practice (the code path is unchanged and was reasoned
through, but not re-run live). **A real click-through pass on the actual
machine, plus a screenshot, is needed before calling this redesign done.**

