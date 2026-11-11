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

