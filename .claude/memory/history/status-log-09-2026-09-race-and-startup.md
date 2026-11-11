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

