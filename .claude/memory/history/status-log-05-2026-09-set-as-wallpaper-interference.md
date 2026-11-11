## Current status (2026-09-12b, "Set as wallpaper" interference - live/runtime case)

Follow-up to the 2026-09-12 entry below: user clarified the report was
about RUNTIME, not restart/reboot - while our video wallpaper is actively
attached right now, picking a `.jpg`/`.png` via Explorer's "Set as
desktop background" doesn't visibly take over, because our window is
still sitting on top of the (correctly-changed, per the entry below's own
proof) result.

**Fix**: `WallpaperManager` now detects a live external wallpaper change
and steps aside automatically. Purely event-driven, no polling:
- `setWallpaper()` snapshots Windows' own static-wallpaper path
  (`WindowsDesktopWallpaper::GetCurrentWallpaperPath()`, new - extracted
  from `RefreshDesktopBackground`) into `m_wallpaperBaselineAtAttach`
  right before attaching. This app never writes that value itself while
  a video wallpaper is active (confirmed by grep: the only
  `SPI_SETDESKWALLPAPER` call anywhere is `RefreshDesktopBackground`'s own
  "re-apply the same value" nudge), so any later difference can only mean
  an external change (Explorer, Settings app, etc).
- `WallpaperManager::nativeEventFilter` (already existed for
  `TaskbarCreated`) now also reacts to `WM_SETTINGCHANGE` - broadcast to
  every top-level window whenever `SystemParametersInfo` is called with
  `SPIF_SENDCHANGE`, which Explorer's own "Set as desktop background"
  uses. On receipt, `onPossibleExternalWallpaperChange()` (new) re-reads
  the current path and, if it differs from the baseline while
  `m_active`, calls `removeWallpaper()` - detaching our window so the
  user's newly-picked wallpaper is actually visible, without requiring
  them to click "Remove Wallpaper" in our own UI first.

**A real bug found and fixed while verifying this live**: the first
version of this fix caused an infinite feedback loop, reproduced directly
(rapid `RefreshDesktopBackground` log spam, one call roughly every
250ms) - `removeWallpaper()` called `RefreshDesktopBackground()` (which
itself broadcasts `WM_SETTINGCHANGE` via `SPIF_SENDCHANGE`) BEFORE
setting `m_active = false`, so our own resultant broadcast looked like
"the user changed the wallpaper again" to
`onPossibleExternalWallpaperChange`'s `m_active` guard and re-triggered
`removeWallpaper()` repeatedly. Fixed by reordering `removeWallpaper()`
to set `m_active = false` (and emit the recovery-state update) BEFORE
calling `RefreshDesktopBackground()` - now that guard correctly ignores
our own broadcast.

**Verified live this session** (not just reasoned through): used the
existing second-instance IPC recovery path (launch the exe twice - the
second launch's `InstanceIpc` hand-off calls
`MainWindow::recoverOrActivate()` → `onSetWallpaper()`) to get a real,
verified-`Active` video wallpaper attached without any UI automation.
Then, via a PowerShell one-off calling `SystemParametersInfoW(
SPI_SETDESKWALLPAPER, ...)` directly (the same API Explorer's own
"Set as desktop background" uses) to simulate the user picking a new
image: the log showed `Detected Windows' own wallpaper changed... -
stepping aside`, immediate clean D3D/DComp shutdown (`shutdown complete,
all D3D/DComp resources released`), exactly one `RefreshDesktopBackground`
afterward (no loop), the process stayed alive/responsive, and
`HKCU\Control Panel\Desktop\Wallpaper` held the test image I'd set,
unmodified by any of this - the exact end-to-end behavior the bug asked
for.

**Not verified this session**: Explorer's actual right-click UI menu
itself (only the underlying `SystemParametersInfo`/`WM_SETTINGCHANGE`
mechanism it uses was exercised directly) and this app's own "Set as
Wallpaper" button click (exercised indirectly via the IPC recovery path
instead, which reaches the same `WallpaperManager::setWallpaper()` call).

## Current status (2026-09-12, "Set as wallpaper" interference - actual root cause + fix)

Follow-up to the 2026-09-11 entry below: the user reported the bug still
reproduced after that fix. Investigation continued and found the REAL
mechanism, which is different from (and supersedes) yesterday's theory.

**Directly tested and disproved yesterday's theory**: with the video
wallpaper live/attached, manually called `SystemParametersInfoW(
SPI_SETDESKWALLPAPER, ...)` from a PowerShell one-off (the exact API
Explorer's own "Set as desktop background" uses internally) - it
**succeeded** (`returned 1`) and the registry's
`HKCU\Control Panel\Desktop\Wallpaper` value updated correctly, even
while our window was attached. This proves Explorer's own wallpaper-set
API was never actually blocked, at any point - not by unthrottled 0x052C
spam (yesterday's fix), not by anything else. The real problem is purely
**visual occlusion**: our opaque DirectComposition window sits in front
of wherever Explorer paints the background, so the OS wallpaper genuinely
changes underneath, the user just can't see it for as long as our window
stays attached.

**The actual root cause**: `MainWindow`'s constructor auto-reattached the
video wallpaper on EVERY fresh process launch (including Start-with-
Windows autostart) whenever `SettingsManager::wasWallpaperActive()` was
true from a previous session - confirmed live: a cold launch's own log
showed `[Lifecycle] Restoring previously-active wallpaper on startup`
firing with no user action that session, followed by a full attach+verify
cycle reaching `Wallpaper recovery COMPLETE`. This directly contradicts
this bug's core rule - "only the explicit Set as Wallpaper action should
activate our wallpaper behavior" - and is exactly what a user perceives
as "the EXE merely running/autostarted permanently blocks Explorer's
wallpaper": the app was silently re-taking the wallpaper on every
relaunch/reboot with no fresh click, covering whatever the user set via
Explorer afterward for as long as that process ran. This was itself a
previously-built, explicitly-requested feature (auto-restore across
Explorer restarts and reboots) - the user confirmed via AskUserQuestion
that it should be removed for cold process starts specifically.

**Fix**: `MainWindow.cpp`'s constructor no longer calls `onSetWallpaper()`
based on `wasWallpaperActive()` - a fresh launch always stops at Ready/
NoVideo and waits for an explicit click, full stop. `wasWallpaperActive`
itself is untouched/still persisted (still used by
`onExitRequested()`/`onRemoveWallpaper()`'s bookkeeping) - only this one
read site was removed. Mid-session recovery is intentionally NOT
affected: `WallpaperManager::onExplorerRestarted()` and
`MainWindow::recoverOrActivate()` (second-instance IPC) both only ever
act while `m_manager->isActive()` is already true, i.e. only continue a
wallpaper THIS running process already explicitly activated - neither
reads `wasWallpaperActive()` or revives state from a past process.

**Also added** (`WindowsDesktopWallpaper::RefreshDesktopBackground()`,
new): re-applies whatever wallpaper Explorer already has configured
(`SPI_GETDESKWALLPAPER` then the same value back via
`SPI_SETDESKWALLPAPER`) - a pure "redraw yourself" nudge, never changes
what the user's wallpaper actually is. Called once from
`WallpaperManager::removeWallpaper()` after detaching (so Explorer
immediately repaints whatever real wallpaper was configured all along)
and once from `MainWindow`'s constructor whenever nothing is being
attached this launch (covering the case where a previous run ended
uncleanly, e.g. `taskkill`, before ever reaching its own detach path).
Verified live: after cold-launching with a previously-attached session's
`wallpaperWasAttached=true` on disk, the log showed zero `[Discover]`/
`[Shell]`/`AttachToDesktop` lines (no attach happened at all) and the
`RefreshDesktopBackground` nudge fired; the registry wallpaper value
(separately set via the PowerShell test above) was confirmed unchanged
by any of this - proving both "no silent re-attach" and "no interference
with whatever Explorer wallpaper is configured."

**Yesterday's fix is still kept** (`FindOrCreateWorkerW`'s per-generation
one-shot spawn-message throttle) - it's a real, independently-justified
fix for a genuine unthrottled-message bug (still worth having even though
it turned out not to be this particular symptom's cause), and is
unrelated to/does not conflict with today's fix.

**Not verified this session** (no way to drive Explorer's right-click
"Set as desktop background" UI or click through this app's own UI from
this tool session): the full end-to-end validation checklist from the bug
report (steps 1-11) on the actual reporting machine - what WAS verified
is the underlying mechanism directly (the SPI call itself, the registry
value, and the log-proven absence of any auto-attach on cold start),
which is what the UI-level symptom was reducible to.

