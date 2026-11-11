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

