# CLAUDE.md

Guidance for Claude Code (or any future agent) working in this repo.

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

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="<path-to-Qt>/6.x/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Must use the MinGW toolchain that ships with the Qt install (e.g.
`<Qt>/Tools/mingw1310_64`), not a system MinGW/MSYS2 — mismatched CRTs
produce `undefined reference to '__imp___argc'` link errors.

Run/test: `build/Motiva.exe`. Logs go to
`%TEMP%\Motiva.log` (via `qInstallMessageHandler` in `main.cpp`) —
always check this file first when diagnosing desktop-attach issues, since
the app has no visible console.

Kill stray instances before rebuilding — a single-instance `QSharedMemory`
lock exists (`main.cpp`), but a running exe still locks the `.obj`/`.exe`
during `cmake --build`:
```bash
taskkill //F //IM Motiva.exe
```

## Current status (2026-09-09, wallpaper visibility investigation - BLOCKED, needs a decision)

User report: the app reports "Active" (frames presenting, `presentedFrameCount` advancing,
attach verified) but the **user-visible desktop still shows the normal Windows/static
wallpaper**, not the video. This means every verification this project has ever done
(`AttachToDesktop()==true`, `presentedFrameCount` advancing) was a proxy, never actual
visual confirmation - see the 2026-09-07 entry's own "NOT yet visually verified" caveat,
which was apparently never resolved by an actual screenshot in any session since.

**New diagnostics added** (read-only, no hierarchy manipulation):
`WindowsDesktopWallpaper::DumpFullHierarchy()` (`WindowsDesktopWallpaper.h`/`.cpp`) - full
Progman/WorkerW/Shell_TrayWnd child-window dump (class, title, PID/TID, parent, owner,
visible/enabled, style/exstyle, window+client rect, `DWMWA_CLOAKED` state), our wallpaper
HWND's exact z-order neighbors, and a targeted probe of the desktop `SysListView32`'s
background color/transparency (`LVM_GETBKCOLOR`/`LVM_GETEXTENDEDLISTVIEWSTYLE`). Called from
`WallpaperManager`'s existing 700ms post-attach verification callback, so it fires exactly
when the app would declare "Active".

**What the diagnostics found** (real run on this machine, `[Hierarchy]` lines in
`%TEMP%\VideoWallpaper.log`):
- Our wallpaper HWND: `IsWindow=true`, `IsWindowVisible=true`, `cloaked=0` (not DWM-cloaked -
  ruling out the one Win32/DWM-level "looks visible but isn't" mechanism that's actually
  queryable), correct parent (Progman), correct size (0,0 to 1920x1080, full virtual desktop),
  correct z-order (`GW_HWNDPREV`=SHELLDLL_DefView directly above us, `GW_HWNDNEXT`=nothing
  below us - we are the bottom-most child of Progman, exactly as designed).
- No other enumerable top-level or child window anywhere in the system resembles a distinct
  "wallpaper image" surface - nothing else is desktop-sized and unaccounted for.
- SHELLDLL_DefView's own `SysListView32`/"FolderView" (which sits directly above us and is
  itself full-screen sized, not just icon-sized): `LVS_EX_TRANSPARENTBKGND=true`,
  `bkColor=CLR_NONE (0xFFFFFFFF)` - i.e. it is explicitly configured with a transparent
  background and no override color. This **refutes** the hypothesis that the icon ListView
  itself paints an opaque wallpaper bitmap over us.
- Confirmed (again) no standalone icon-less WorkerW exists on this system at all -
  `SHELLDLL_DefView`'s owner is Progman itself, not a separate WorkerW; every enumerated
  `WorkerW` is either 166x47 or 0x0 (decoys), matching every prior session's finding.

**Conclusion so far**: every Win32/DWM-cloak-level signal available says our window is
correctly attached, positioned, and uncloaked - yet the user does not see it. The remaining,
unrefuted explanation is that on this Windows build, DWM composites the actual desktop
wallpaper bitmap through a mechanism tied specifically to a genuine icon-less WorkerW (the
thing the removed 0x052C spawn-message dance used to create), not to "any child of Progman
occupying the right Win32 z-slot." The Progman-direct-attach fallback this app has always
used when no real WorkerW exists was, per its own code comments, a "far better than nothing"
best-effort guess that was **never actually visually confirmed** before now - it apparently
does not work as a substitute on this machine's Explorer/DWM version, independent of and
unrelated to the 2026-09-09 startup-delay fix (that fix only changed *when* this same
fallback is reached, not *what* it does - the fallback itself, and its apparent failure to
actually occlude the OS wallpaper, predates this session).

**Blocked**: the one concrete, evidence-aligned next step - attempting to obtain a genuine
icon-less WorkerW - requires the 0x052C spawn message, which this session's own instructions
explicitly forbid reintroducing (for good reason: it was the source of the original
multi-second startup delay). No other Win32-legal mechanism to create/obtain a
DWM-recognized wallpaper host was identified that fits the constraints (no fake
fullscreen/topmost window, no continuous z-order manipulation, no GDI). A live screenshot to
visually confirm any hypothesis further was attempted (`System.Windows.Forms`/
`System.Drawing` screen capture via PowerShell, saved to `%TEMP%\desktop_screenshot.png`)
but the interactive session was locked (PIN entry screen) at the time - not attempted to
unlock (no credentials, and doing so would be inappropriate regardless). This needs either a
user decision (permission for a *bounded, one-shot* spawn attempt - not the old
multi-second-polling version - at attach time only) or a user-provided screenshot of the
actual unlocked desktop with the app "Active" to confirm/refute the hypothesis further.

## Prior status (2026-09-09, startup-delay elimination)

Follow-up to the entry below, whose own instrumentation identified the
exact bottleneck it was built to find: `startupToWallpaperAttached` was
dominated almost entirely by `WindowsDesktopWallpaper::FindOrCreateWorkerW`'s
0x052C spawn-message dance (send message, `Sleep(50)`-poll up to 1s,
repeat for 3 variants, re-apply wallpaper + `Sleep(300)`, repeat the 3
variants again) - measured at 6693ms end-to-end on this dev machine, where
no standalone icon-less WorkerW is ever created, so every one of those
sends/sleeps was pure waste before falling through to the Progman fallback
anyway.

**Fix**: removed the spawn-message dance from the hot path entirely.
`FindOrCreateWorkerW` now returns the Progman fallback immediately the
moment the cheap probe (no sleeps, no message sends) fails to find a
standalone WorkerW - see its updated comment. This is safe because
`AttachToDesktop`'s z-order-targeting logic (SetWindowPos behind
`SHELLDLL_DefView`) already produces an identical, fully-correct visual
result on Progman as on a real WorkerW (established during the
2026-09-07 DirectComposition rewrite below) - there was no correctness
reason to spend seconds chasing the "nicer" host first. The
generation-scoped discovery cooldown added in the entry below
(`s_lastFailedDiscoveryGeneration`) became dead code once there was
nothing expensive left to cache/cool down, and was removed along with it.
`FindOrCreateWorkerW`/`AttachToDesktop` keep their `generation` parameter
(now unused, `Q_UNUSED`) rather than touching call sites again for no
benefit.

**New StartupDiagnostics marks**: `desktopDiscoveryStart`,
`requiredShellWindowFound`, `attachToDesktopStart`/`attachToDesktopEnd`,
`wallpaperHwndCreated`, `rendererInitStart` - plus two new derived deltas,
`discoveryToAttached` (isolates desktop discovery+reparent latency from
video/decoder startup) and `attachDuration` (single `AttachToDesktop` call
length), both logged in the "Startup timing summary" and persisted into
`RecoveryState`'s `startup{}` block.

**Measured results on this dev machine** (3 consecutive launches after the
fix, `%TEMP%\VideoWallpaper.log`):
- Run 1 (first launch after rebuild): `startupToWallpaperAttached=3222ms` -
  `FindOrCreateWorkerW` itself logged `took 0 ms`, but the subsequent
  `SetParent`/`SetWindowPos` calls inside `AttachToDesktop` took ~2.9s this
  one time (plausibly one-off DWM/AV first-touch cost on a freshly linked
  binary - not reproduced on runs 2/3, see below - flagged here rather
  than hidden).
- Run 2: `startupToWallpaperAttached=528ms`, `discoveryToAttached=275ms`,
  `startupToFirstPresentedFrame=635ms`.
- Run 3: `startupToWallpaperAttached=623ms`, `discoveryToAttached=371ms`,
  `startupToFirstPresentedFrame=728ms`.

Down from the previous entry's baseline of **6693ms** to consistently
**~500-700ms** end-to-end (video decoding/presenting internally in
~300-600ms of that, running in parallel with the now near-instant desktop
attach) - roughly a 10-13x reduction, and no longer dominated by desktop
discovery at all. A single Explorer restart was re-tested after this
change and still recovered correctly (`presenting=true`) in under 1s
(925ms detected-to-verified-complete, including the existing 700ms
verification wait), confirming no regression to Explorer-restart recovery
or the generation/single-flight machinery. Second-instance IPC
(`InstanceIpc`) was not touched this session and remains fully independent
of the discovery path.

**Not eliminated, and not a target for further shortening here**: video
decode/first-presented-frame latency itself (~300-700ms, `QMediaPlayer`
opening the file and producing a first `QVideoFrame` plus the D3D upload
chain) - this is normal decoder startup work, not a "wait", and reducing
it further would mean touching `VideoPlayer`/`D3DWallpaperRenderer`'s
actual decode/render pipeline, which was explicitly out of scope
(architecture must not change). The one remaining variable is the
occasional one-off `SetParent`-side delay seen in Run 1 above - not
reproduced on repeat runs, so not treated as a real bottleneck without
further evidence; the new `attachDuration` mark will make it immediately
visible again if a user ever reports it recurring.

## Prior status (2026-09-09, persistent diagnostics + second-instance IPC + Explorer-restart cooldown fix)

Follow-up to the entry below, addressing three real-machine issues: a
30-60s startup video delay, Explorer-restart recovery eventually failing
after ~2+ restarts, and double-click-while-running requiring End Task.

**New files**: `RecoveryState.{h,cpp}` (small persistent JSON state at
`%LOCALAPPDATA%\VideoWallpaper\VideoWallpaper\recovery-state.json` -
`QStandardPaths::AppDataLocation`, atomic write via temp-file +
`MoveFileExW`, never treated as authoritative for live attach state -
see its header comment), `StartupDiagnostics.h` (header-only singleton
recording named startup checkpoints with millisecond timestamps since
process start, computing `startupTo*` deltas), `InstanceIpc.{h,cpp}`
(message-only-window + `WM_COPYDATA` second-instance hand-off).

**Root cause found and fixed** (repeated-Explorer-restart failures):
`WindowsDesktopWallpaper::FindOrCreateWorkerW`'s negative-discovery
cooldown (added in the 2026-09-08 entry below) was keyed on a flat
5-minute *wall-clock* timer that was never reset on Explorer restarting -
if any restart cycle hit a failed discovery once, every restart for the
next 5 minutes silently got the (less reliable) Progman fallback instead
of ever retrying real WorkerW discovery. Fixed by scoping the cooldown to
the caller's attach `generation` (bumped on every Explorer restart -
`WallpaperManager::m_attachGeneration`) instead of time:
`s_lastFailedDiscoveryGeneration`/`s_hasFailedDiscovery` replace
`s_lastFailedDiscovery`. `FindOrCreateWorkerW`/`AttachToDesktop` both now
take a `uint64_t generation` parameter.

**Real bug found AND reproduced live** (not just theorized): during this
session's own 5-cycle spaced `taskkill /F /IM explorer.exe` test, the 5th
restart's `TaskbarCreated` broadcast was never received by this process at
all - no polling fallback caught it either, matching the user's exact
"process alive, wallpaper dead" report. Confirmed real (not a race in the
test harness) by then launching a second `VideoWallpaper.exe` instance,
which via the new IPC hand-off called `WallpaperManager::recoverOrActivate()`
on the existing instance and successfully re-attached
(`[Verify] ... presenting= true`) without ever requiring End Task. This is
the concrete justification for shipping the IPC recovery path rather than
only fixing detection: `TaskbarCreated` delivery is apparently not fully
reliable even when Explorer's restart itself succeeds, so a
detection-independent recovery trigger is needed as a safety net, not just
a nicety.

**Second-instance IPC**: `main.cpp`'s `QSharedMemory` single-instance lock
is unchanged (exactly one process ever runs). A losing second launch now
calls `InstanceIpc::sendRecoverRequest()` (finds the winner's message-only
window by class name, sends `WM_COPYDATA`) instead of silently exiting.
The winner's `InstanceIpc::startListening()` (called from `MainWindow`'s
constructor) receives it and emits `recoverRequested()` (queued connection)
→ `MainWindow::recoverOrActivate()`, which calls
`WallpaperManager::recoverOrActivate()` if a wallpaper is active (re-drives
the existing single-flight/generation-guarded `attachAllWindows()` +
700ms verification pipeline - no new recovery logic, reuses the
Explorer-restart one) or `onSetWallpaper()` if none was ever attached, then
brings the settings window forward either way.

**Real bug found in the pre-existing shared log file handler** (not part of
the plan, found via testing the above): `main.cpp`'s `fileLogHandler`
opened its `QFile` in `QIODevice::Append` mode, but Qt only seeks to
end-of-file once, at `open()` time - it does not reseek before every
write. With two processes each holding their own handle open to the same
growing log file (the losing second-instance process, briefly, before it
exits), one process's write can land at an offset that was EOF *when it
opened the file* but is now mid-file by write time, corrupting/interleaving
lines (observed directly: a second process's line landed spliced into the
middle of an existing one, e.g. two timestamps glued together with no
newline between them). Fixed with an explicit `logFile.seek(logFile.size())`
immediately before every write. Also added a short retry loop (5×20ms) to
both this file's `QFile::open()` and `RecoveryState::save()`'s
`MoveFileExW` call after observing occasional transient
sharing-violation/`ERROR_ACCESS_DENIED` failures in testing - in both cases
the previous file content is left untouched by design if all retries fail,
never corrupted.

**Startup-delay investigation status**: instrumentation shipped and
verified working (`StartupDiagnostics` marks + `RecoveryState`'s
`startup{}` block, logged as a "Startup timing summary" every launch and
echoed as the *previous* session's numbers on the next one). On this dev
machine, real numbers from a normal launch: `startupToExplorerReady=92ms`,
`startupToVideoReady=325ms`, `startupToFirstFrame=395ms`,
`startupToFirstPresentedFrame=454ms`, but
**`startupToWallpaperAttached=6693ms`** - i.e. video is decoding and
presenting internally within half a second, but the desktop attach itself
(`WindowsDesktopWallpaper::AttachToDesktop` → `FindOrCreateWorkerW`) takes
~6.7s on this machine because it falls through the full spawn-message
dance and ultimately lands on the Progman fallback (no standalone WorkerW
exists on this Explorer build - consistent with every prior session's
findings in this file). This machine's ~6.7s is the same *category* of
delay as the user's reported 30-60s, just a smaller magnitude (less boot
contention here); the instrumentation is what will show whether the user's
real number is dominated by this same discovery sequence or something
else - **the user's own Windows-restart test with this build is still
needed to confirm the exact number for their machine**; no changes were
made to `FindOrCreateWorkerW`'s retry/timing logic itself this session
(deliberately - see the entry below for why guessing here previously
wasted a session).

**Diagnostics added per Explorer restart**: `onExplorerRestarted()` now
logs old/new Explorer PID and the current attach generation; both the
pre-recovery and post-attach dumps now also log
`D3DWallpaperRenderer::hasValidDCompState()`/`hasValidDevice()` (two new
small diagnostics-only accessors reading existing pointers, no new
resources) and `presentedFrameCount` per window, via new
`WallpaperWindow::rendererHasValidDCompState()/rendererHasValidDevice()`
passthroughs.

**Verified locally this session** (see file for exact log lines): normal
launch (recovery-state.json created, startup deltas logged and persisted);
5 spaced Explorer restarts (generations 1-4 detected/recovered correctly,
cycle 5 missed detection - see above - and was recovered via IPC instead);
second-instance launch while running (no second process persists, correct
`[IPC]` log lines both sides after the log-corruption fix, existing
instance recovered without End Task); unclean termination via
`taskkill /F` correctly leaves `cleanExit=false` on disk, picked up and
logged as a "Previous session: ..." summary on the next launch. **Not
verified this session**: the tray menu's "Exit" action specifically (this
tool session has no way to click a native system tray context menu - same
limitation noted in the 2026-09-07 entry below) - `onExitRequested()`'s
`m_recoveryState.setCleanExit(true)` was verified by code reading only,
not by an actual tray-Exit click; and the real Windows-restart startup
delay (needs the user's own one-time reboot, per above).

## Prior status (2026-09-09, Explorer-restart race fix + real presentation verification)

Follow-up to the entry below. User-reported real-machine behavior after
that fix: the process now DOES survive Explorer restarts (correct PID,
never needs End Task), but the wallpaper sometimes stopped actually
rendering after a restart even though the process stayed alive - "attach
succeeded" was being trusted as "recovery succeeded" without ever
checking whether frames were still reaching the screen.

Root cause found by code review (a real bug, though not reproduced
crashing on this dev machine - data races are timing-dependent):
`WallpaperWindow::onRebindFinished()` was calling
`WindowsDesktopWallpaper::AttachToDesktop()` **directly on the GUI
thread**, while `WallpaperManager`'s async attach path (from the previous
fix) could call the *same* function **concurrently on a QtConcurrent
worker thread**. Both paths read/wrote the same non-atomic module-level
globals in `WindowsDesktopWallpaper.cpp` (`g_attachedHost`,
`g_attachedProgman`, `g_hasAttached`, plus a function-local `static`
cooldown timestamp) with no synchronization - a genuine data race /
undefined behavior that could corrupt that state or the discovery
sequence under real timing, independent of whether it visibly crashed.

Fixes, in order of how load-bearing they are:
- **Single call site**: `WallpaperWindow::onRebindFinished()` no longer
  calls `AttachToDesktop()` - it only recreates the HWND / rebinds the
  DComp target and emits `readyForReattach(bool)`
  ([WallpaperWindow.h](src/WallpaperWindow.h)/[.cpp](src/WallpaperWindow.cpp)).
  `WallpaperManager` connects that signal to `onWindowReadyForReattach()`,
  which is the only thing that calls `attachAllWindows()`
  ([WallpaperManager.cpp](src/WallpaperManager.cpp)) - there is now
  exactly one call site for `AttachToDesktop()`/`DetachFromDesktop()` in
  the whole app.
- **Mutex as defense in depth**: both functions in
  [WindowsDesktopWallpaper.cpp](src/WindowsDesktopWallpaper.cpp) now hold
  a `std::mutex` (`g_desktopStateMutex`) for their entire body, so even a
  future second call site can't reintroduce this race.
- **Generation tokens**: `WallpaperManager` tracks `m_attachGeneration`
  (bumped on Explorer restart and whenever the window set is torn down)
  and `m_attachJobGeneration` (captured when a background attach job
  starts). `onAttachAttemptFinished()` discards a job's result outright if
  the generation moved on while it was running - a stale async attach can
  no longer reparent to an old HWND, restore an old WorkerW, or flip
  lifecycle state back to "attached" after Explorer has since restarted
  again. `m_shuttingDown` (set first thing in `~WallpaperManager`) plus
  `m_attachWatcher.waitForFinished()` in the destructor make sure no
  background job outlives the objects it would otherwise touch.
- **Real presentation verification, not just "SetParent succeeded"**:
  `D3DWallpaperRenderer` now exposes an atomic `presentedFrameCount`
  (incremented once per successful `Present()`,
  [D3DWallpaperRenderer.cpp](src/D3DWallpaperRenderer.cpp)). After a
  successful attach, `WallpaperManager` snapshots each window's frame
  count, nudges the renderer to `recommitAfterReparent()` (an extra
  `IDCompositionDevice::Commit()` call *after* the SetParent into the
  desktop hierarchy - cheap, and rules out any DWM-side composition state
  that specifically needs a post-reparent commit), then checks back after
  700ms and only logs `Wallpaper recovery COMPLETE - ... Video=PLAYING
  Present=ACTIVE` (or `INCOMPLETE`/`NOT ADVANCING`/`STALLED`, which
  re-arms the retry timer exactly like a failed attach) once frames have
  actually advanced. `WindowsDesktopWallpaper::DumpDesktopState()` (new)
  logs the live Progman/SHELLDLL_DefView/WorkerW hierarchy and the
  wallpaper HWND's actual `GetParent`/`IsWindow`/`IsWindowVisible`/style,
  called before recovery starts and again right after attach, always
  re-derived from live Win32 queries (never from this module's own
  cached state) - this is what would show, in a future repro, exactly
  which shell object the wallpaper ended up parented under.

Verified live on this machine: 3 real, individually-spaced
`taskkill /F /IM explorer.exe` + relaunch cycles, each one recovering
with genuine per-cycle confirmation - `[Verify] window 0 framesBefore=X
framesAfter=Y presenting=true` followed by `Wallpaper recovery COMPLETE`
- and the same `VideoWallpaper.exe` PID throughout all three. One
caveat worth recording: an earlier *rapid* back-to-back loop (three
restarts only ~6s apart, `explorer.exe &` launched from a non-interactive
shell) produced only one recovery log entry for three restarts - almost
certainly the loop racing ahead of Explorer actually finishing its own
restart before the next `taskkill`, not a bug in this app (a slower,
individually-verified loop recovered every single time). If a genuinely
rapid multi-restart scenario ever needs testing again, space each cycle
out and confirm the previous one's `TaskbarCreated message received` log
line appeared before starting the next.

**Not independently reproduced on this dev machine**: the user's exact
original report (process alive, wallpaper silently dead, no crash) -
every real Explorer restart performed *from this tool session* recovered
correctly both before and after this fix. The data race above is a real,
confirmed bug that is a plausible explanation and is now closed either
way, but if the user still sees wallpaper failing to return after
Explorer restart with this build, the new `[Diag]`/`[Verify]` log lines
in `%TEMP%\VideoWallpaper.log` should pinpoint exactly which object (
Progman vs. WorkerW vs. wallpaper HWND parentage vs. DComp Commit vs.
actual frame presentation) is the one still pointing at the old desktop.

## Prior status (2026-09-08, startup/recovery ~2min delay fix)

Root cause found and fixed (do not reintroduce): `WindowsDesktopWallpaper::
AttachToDesktop()` (Progman/WorkerW discovery: `SendMessageTimeoutW` +
`Sleep`-based waits for Explorer to react, doubled by the "re-apply
wallpaper and retry" fallback) was being called **synchronously on the GUI
thread** - from `WallpaperManager::attachAllWindows()`, itself called
directly out of `MainWindow`'s constructor for startup and out of
`onExplorerRestarted()`/the render-thread rebind callback for recovery.
On this dev machine (Explorer here never creates a real icon-less
WorkerW - confirmed by testing) that full discovery sequence measured
**7.4s** by itself even under light load, run entirely before
`MainWindow`'s constructor even returned (i.e. before `app.exec()`
started the Qt event loop). Under real boot-time contention (antivirus
scanning the fresh EXE, other startup apps, Explorer's own shell
extensions loading) the exact same fixed sequence of waits is what
stretches into the ~2 minute delay the user reported both after a cold
boot and after killing/restarting `explorer.exe` - not a hardcoded 2min
constant anywhere in the code, but this synchronous discovery having no
upper bound and blocking the one thread needed to keep the app itself
responsive (and, during that window, to receive Explorer's own
`TaskbarCreated` broadcast).

Fix: `WallpaperManager::attachAllWindows()` now runs
`WindowsDesktopWallpaper::AttachToDesktop()` via `QtConcurrent::run` on a
worker thread (`m_attachWatcher`/`m_attachInFlight` in
`WallpaperManager.h`) and only marshals the boolean result back to the
GUI thread - no blocking wait anywhere on the GUI/render threads. A
short (300ms), self-terminating poll (`m_attachRetryTimer`) re-kicks the
(non-blocking) attach attempt if the desktop hierarchy wasn't ready yet,
stopping the instant it succeeds; this is a bounded "wait for a
condition" loop, not a periodic reattach/health-check loop (that pattern
was previously removed as a z-order/flicker cause - see
`WindowsDesktopWallpaper::NeedsReattach`'s comment, and it's still dead
code / never called). No arbitrary 30/60/120s timeouts were added, no
`HWND_TOPMOST`, no continuous recreate/reparent, and the DirectComposition/
D3D11 renderer and video decoder were not touched.

Verified via this tool session's own build+run (see git log for the exact
diff): `MainWindow` construction dropped from **7.4s** to **~106ms**
wall-clock (log timestamps: `MainWindow construction complete` now logs
almost immediately, while `[Discover]`/`[Shell] AttachToDesktop ... took
6546 ms` continues in the background afterward) - i.e. startup is no
longer blocked on Explorer discovery at all, whatever it ends up taking.
Killing and restarting `explorer.exe` was measured recovering in
**~318ms** (`[Shell] Desktop hierarchy became ready after 318 ms`), with
the `VideoWallpaper.exe` process's PID unchanged throughout (never froze,
never needed End Task, never needed relaunch). Timestamped diagnostic
logging (millisecond precision) was added across the full chain -
`MainWindow`/`WallpaperManager` lifecycle, `FindOrCreateWorkerW` discovery
stages, `AttachToDesktop`, DirectComposition `initialize()`/`rebindToWindow`
- to make any future regression visible directly in
`%TEMP%\VideoWallpaper.log` without needing to re-instrument.

**Not fixed / out of scope for this pass:** the underlying
`FindOrCreateWorkerW()` discovery sequence itself can still take multiple
seconds *of wall-clock time* (now off the GUI thread, so this no longer
freezes the app, but the wallpaper still visibly takes that long to
appear) on machines/builds where Explorer never creates a real icon-less
WorkerW. Shortening that sequence itself (fewer `0x052C` variants, less
`Sleep`-based polling) was out of scope here per explicit instruction not
to touch retry-interval constants beyond fixing the actual GUI-thread
block; worth revisiting only with fresh live measurements if the
background-thread version is still perceived as slow to appear.

## Prior status (2026-09-08, Explorer-recovery deadlock fix)

The DirectComposition rewrite below works, but the Explorer-restart
recovery it shipped with had a real deadlock: recovery used
`Qt::BlockingQueuedConnection` to call the renderer's rebind method from
the GUI thread. If a `presentFrame()`/`Present()` call was already in
flight on the render thread exactly when Explorer destroyed the old
DirectComposition target, that call could take an indeterminate time to
return, freezing the GUI thread until it did - matching every symptom the
user reported in real usage (process "running" but fully unresponsive,
had to End Task, second launch did nothing because the frozen process
still held the single-instance lock). Fixed by making the rebind
asynchronous (`rebindFinished(bool)` signal instead of a blocking return
value) and by always destroying+recreating the native window on recovery
rather than trusting `IsWindow()` on the old handle (its numeric value can
be reused by one of the many windows Explorer creates moments later during
its own restart).

Verified via 6 total Explorer restarts across two separate test runs (5
back-to-back, then 1 more after a clean relaunch), all via `--autostart`:
every cycle recovered within about a tick, process never went
"Not Responding", and live `EnumChildWindows` diagnostics confirmed
correct z-order (`SHELLDLL_DefView` -> our window) under the new Progman
each time. Second-instance-while-first-running retested clean.

**Open, unconfirmed item:** once, shortly after the 5-rapid-cycles run
completed, the process was found gone with no Windows Error Reporting
crash record for it at all (checked the Application event log - nothing).
Not reproduced on a subsequent clean retest of the same scenario. Possible
directions if it recurs: resource accumulation across many rapid
recreate-HWND-and-DComp-target cycles (`CreateTargetForHwnd`/
`CreateWindowExW` calls, one pair per restart, are not obviously leaked by
code inspection but were not specifically stress-tested for leaks), or
something outside this app's control given the artificially rapid,
back-to-back Explorer kill/restart cycling used for testing (not
representative of how Explorer actually restarts in normal use). Worth
a longer soak test (many cycles, minutes apart) before considering this
closed.

## Prior status (2026-09-07, DirectComposition rewrite)

Presentation was rewritten from GDI (`WM_PAINT`/`StretchDIBits`) to
DirectComposition + D3D11 (`D3DWallpaperRenderer`), driven by a live
diagnostic finding: `Progman` on this machine carries
`WS_EX_NOREDIRECTIONBITMAP`, meaning it opts out of classic DWM
bitmap-redirection compositing - which is almost certainly why a
geometrically-correct GDI child window (verified via `GetWindowRect`/
`IsWindowVisible`, not just a screenshot) never actually rendered. The
render window now carries the same extended style and presents through a
DirectComposition visual instead.

The top-level-sibling-behind-Progman fallback (from the previous session)
was removed entirely - live diagnostics proved it placed the window
**above** Progman/icons in the global z-order (the opposite of what was
wanted) and required periodic z-order upkeep that caused flicker. The app
now always attaches as a real child of Progman, positioned behind
`SHELLDLL_DefView`, exactly once, with no further HWND manipulation during
playback (verified via extended log observation - zero repeated
reattach/SetWindowPos activity over 2+ minutes of runtime).

A custom standalone diagnostic tool (`diag.exe`, built from
`diag.cpp` in the session scratchpad, not part of this repo) confirmed via
live `EnumChildWindows` that after attaching, Progman's children are
ordered front-to-back: `SHELLDLL_DefView` (icons) → our render window →
a genuine, full-desktop-sized, **visible** `WorkerW` that Explorer created
as a **child of Progman** (not the traditional top-level sibling the
existing `FindWorkerWBehindIcons` search logic looks for - a likely
follow-up: teach that search to also check for a WorkerW nested under
Progman, not just top-level siblings). That ordering - icons in front,
video in the middle, backdrop WorkerW at the back - is exactly the
layering a wallpaper needs.

**What is NOT yet visually verified** (this tool session has no way to see
the live desktop - only screenshots/Task Manager reports from the user
establish ground truth):
- Whether the video actually renders on screen now (the architecture and
  z-order are right per diagnostics; DirectComposition initialization
  logs no errors; but "renders" has been wrong before despite correct
  structure - it must be confirmed by an actual screenshot).
- Icon/taskbar interactivity, Win-key behavior, playback smoothness,
  Explorer-restart recovery, multi-monitor (this machine only ever showed
  one 1536x864 or 1920x1080 display across sessions - never tested with
  two).
- Clean shutdown via the tray "Exit" menu item specifically - this tool
  session cannot click a system tray context menu; WM_CLOSE was confirmed
  to correctly minimize-to-tray (by design), which is not the same test.

## Prior status (2026-09-07, pre-DirectComposition) — partially fixed, needs live re-verification

A second pass fixed several concrete, verified-in-code bugs (see git log for
the full diagnosis/fix commit): the GUI-thread frame conversion and slow
HALFTONE blit (playback smoothness/lag), the tray-Exit path skipping
cleanup (process lingering after close), and — critically — a regression
where the WorkerW-discovery probe (several seconds of Sleep-based waiting)
was being re-run by the 3s health-check timer on every tick once it always
failed, making the whole app go "Not Responding" indefinitely. That's now
cooldown-gated to once per 5 minutes.

**Still not fixed / needs your live verification:**
- The WorkerW-discovery probe still runs *synchronously on the GUI thread*
  the (now rare) times it does run - it should be moved to a background
  thread with only the final `SetParent`/`SetWindowPos` marshaled back,
  per the user's explicit "don't block the GUI thread" requirement. Not
  done yet due to time constraints in that session.
- Video-appears-only-after-Win-key and taskbar/icons-covered-while-visible
  (issues D/E/F) were **not** re-verified after these fixes - the WorkerW
  discovery message-toggle fix might have changed nothing on this specific
  machine (it still falls back to Progman every time in testing). Get a
  fresh screenshot and Task Manager check before assuming these are fixed.
- Playback smoothness improvements (moving conversion off the GUI thread,
  faster blit mode) are implemented and build cleanly but were not visually
  verified - this tool session cannot see the interactive desktop.

## Prior status (2026-09-07, original) — NOT reliably working

Despite an earlier README claiming "confirmed working end-to-end", live
testing on the actual dev machine (Windows 11, build `10.0.26200`, an
unusually high/recent — likely Insider/Dev-channel — build number) surfaced
real, reproducible bugs. **Do not trust old status claims in README.md
without re-verifying on the current machine; update them as bugs are
found/fixed.**

Root cause: this Explorer build never spawns a real, DWM-recognized
icon-less `WorkerW`, even after the standard fix of forcing Explorer to
rebuild its desktop pipeline via `SystemParametersInfoW(SPI_SETDESKWALLPAPER,
...)` before retrying (see `WindowsDesktopWallpaper::FindOrCreateWorkerW`).
All `WorkerW`-class windows found on this system are 166×47 decoys unrelated
to the desktop.

Without a genuine WorkerW, `AttachToDesktop()` falls back to a top-level
sibling window pinned behind `Progman` in the global z-order
(`isProgmanFallback` branch). This fallback is unstable:

- **Bug 1**: video stays invisible until something (observed: pressing the
  Windows key) nudges Explorer/DWM into recompositing.
- **Bug 2**: was periodic flicker from `checkWorkerWHealth()` unconditionally
  re-attaching every 3s tick; fixed to only re-pin when the window has
  actually drifted out of position (`GetWindow(hwnd, GW_HWNDPREV) != progman`).
  Re-verify this actually eliminated the flicker on real hardware.
- **Bug 3**: taskbar and desktop icons get covered while the video is
  visible — consistent with the window occasionally getting promoted above
  everything (not just above the backdrop) rather than staying wedged
  between backdrop and icons, since there's no stable "wedge" z-slot without
  a real WorkerW.

All three point at the same underlying issue: on this Explorer build there
is no reliable middle z-position between the OS wallpaper backdrop and the
desktop icons/taskbar for a foreign window to occupy. This may be a genuine
platform limitation of this specific (possibly Insider) Windows build rather
than something fixable purely in application code — next session should
re-test after Windows updates, and/or investigate whether a newer,
documented API (e.g. anything under `IShellWallpaperHost` or similar,
introduced in recent Windows 11 builds) exists as a replacement for the
`0x052C` hack before sinking more time into z-order guessing.

Diagnostic pattern that worked well: temporarily add `qWarning()` calls
dumping `GetWindowRect`/`GetClientRect`/`IsWindowVisible`/`GetParent` around
`AttachToDesktop`, rebuild, run, read `%TEMP%\VideoWallpaper.log` — this
tool session cannot see the interactive desktop directly (`FindWindow`
returns null from this shell's session), so screenshots from the user are
the only way to visually verify rendering.

## Source layout (2026-09-12 reorganization)

Source now lives under `scripts/`, split by role rather than all in one
`src/` folder:
- `scripts/src/*.cpp` — implementation files.
- `scripts/include/*.h` — headers (declarations only, no `.cpp` needed for
  header-only ones like `StartupDiagnostics.h`).
- `scripts/main/main.cpp` — the ONLY executable entry point (`int main`) in
  the whole project; everything else, even files only the exe uses, is an
  implementation file and stays in `scripts/src/`, not here.

This was a pure file-organization move - `CMakeLists.txt`'s
`target_include_directories(Motiva PRIVATE scripts/include)` is the only
reason plain `#include "Foo.h"` still resolves from every `.cpp`; no
source file's `#include` lines needed to change (none used a path prefix
before or after). No class, behavior, or build flag changed. Old paths in
the dated status entries below (e.g. `src/WindowsDesktopWallpaper.cpp`)
are historical and were left as-is; this Code map section is the
up-to-date reference.

## Code map

- `scripts/include/WindowsDesktopWallpaper.h` / `scripts/src/WindowsDesktopWallpaper.cpp`
  — ALL raw Win32 desktop-attach logic (Progman/WorkerW lookup, SetParent,
  z-order). Nothing else should call these Win32 APIs directly.
- `scripts/include/WallpaperManager.h` / `scripts/src/WallpaperManager.cpp`
  — orchestrates the video player + one `WallpaperWindow` per monitor +
  the attach/health-check loop.
- `scripts/include/WallpaperWindow.h` / `scripts/src/WallpaperWindow.cpp`
  — plain native Win32 window (own `WndProc`), deliberately **not** a
  `QWidget` — a reparented `QWidget`'s `paintEvent` was confirmed to never
  fire after `SetParent` (Qt's QPA doesn't know about the reparent). Raw
  GDI `StretchDIBits` in `handlePaint()`.
- `scripts/include/VideoPlayer.h` / `scripts/src/VideoPlayer.cpp` —
  `QMediaPlayer`/`QVideoSink` decode pipeline, shared (one decode) across
  all monitor windows via `std::shared_ptr<QImage>`.
- `scripts/include/MainWindow.h` / `scripts/src/MainWindow.cpp` — UI +
  tray + in-app preview (normal QWidget, no reparenting, not subject to
  the paintEvent issue above).
- `scripts/include/SettingsDialog.h` / `scripts/src/SettingsDialog.cpp` —
  the settings window (scaling/monitor/volume/mute/loop/start-with-
  Windows), split out of `MainWindow` during the UI redesign.
- `scripts/include/SettingsManager.h` / `scripts/src/SettingsManager.cpp`
  — `QSettings` (registry, `HKCU\Software\Motiva`) for user-facing
  preferences (video path, volume, scaling, etc).
- `scripts/include/RecoveryState.h` / `scripts/src/RecoveryState.cpp` —
  small persistent JSON diagnostic/recovery state, separate from
  SettingsManager (per-user AppData file, not registry) - see the
  2026-09-09 entry above. Never authoritative for live attach state.
- `scripts/include/StartupDiagnostics.h` — header-only process-wide
  startup checkpoint timeline singleton.
- `scripts/include/InstanceIpc.h` / `scripts/src/InstanceIpc.cpp` —
  second-instance recovery hand-off (message-only window + `WM_COPYDATA`),
  separate from the `QSharedMemory` single-instance lock in
  `scripts/main/main.cpp`.
- `scripts/main/main.cpp` — the executable entry point (`int main`):
  logging setup, single-instance lock, autostart flag handling,
  `MainWindow` construction.
