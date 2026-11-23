# Performance intelligence (opt-in, additive)

Status: client side implemented and live-verified 2026-11-19 (scratch build, see "Verification" below).
NOT production-ready: NO production backend and NO production signing key exist (verified: none in repo).

## What it is
Observer + signed tuning profiles layered beside the wallpaper engine. The engine never waits on it; with
the setting off, offline, or with no profile, behaviour is the pre-existing hard-coded defaults.

| File | Role |
|---|---|
| `PerfProfile.h/.cpp` | Param allowlist + ranges, envelope parse, ECDSA P-256/SHA-256 verify (Windows BCrypt, no new dependency), rule resolve. Placeholder public key. `kProductionKeyConfigured=false` |
| `ProfileStore.h/.cpp` | Active/previous profile files + `state.json` under `<AppLocalData>/perf-intel/`. `value(Param)` = lock-free atomics. install / rollback / trial |
| `PerfProbe.h/.cpp` | Consent-gated async recorder (own 1-thread pool), bounded `perf-records.jsonl` (500 lines), sanitising, de-dupe, hardware class (DXGI vendor + RAM GB) |
| `ProfileSync.h/.cpp` | Async QNetworkAccessManager: POST `/v1/records`, conditional GET `/v1/profile`. Rate limited (6 h upload, 24 h poll), 15 s timeout, silent failures |
| `VideoApplyTelemetry.h/.cpp` | Qt-only lifecycle tracker (tokens, loadSeq metadata, exactly-once resolve); injectable sink |
| `scripts/tools/mock-perf/` | `mock_server.py` (loopback mock), `make_profiles.py` (valid + invalid signed profiles), `dev_private_key.pem` (DEV ONLY) |
| `scripts/tests/PerfIntelligenceTests.cpp` | Opt-in test target (`scripts/tests/CMakeLists.txt`), needs Python + `cryptography` |

## Existing code touched (minimal)
- `WallpaperManager.cpp`: post-attach verify wait (default 700 ms) and attach retry interval (default 300 ms)
  now read `ProfileStore::value()` at use time. No logic change.
- `WebMediaCache.cpp`: download transfer timeout (default 60000 ms) reads `ProfileStore::value()`.
- `MainWindow.cpp`: one init block after `WebMediaCache` creation; `recordPerf()` calls at "media ready" and
  "apply confirmed/failed" in `onBrowserRequest`; `confirmHealthy()` on `wallpaperVerified` / `aboutToQuit`.
- `SettingsManager` (`intelligence/optIn`, default false) + `SettingsDialog` General toggle.
- `CMakeLists.txt`: new sources, `bcrypt` link. `VideoPlayer.*` only gained passive metadata + `loadSeq`/`LoadOrigin` (no decode/render change).

## Rules the code enforces
- Allowed params: `verifyWaitMs` 300-1500, `attachRetryMs` 150-1000, `downloadTimeoutMs` 15000-120000.
  Unknown param, non-integer, out of range, bad/missing signature, wrong schema, expired, not yet valid,
  >64 KB, >32 rules => whole profile rejected, active values unchanged.
- New profile must have a higher version and not be in the persisted bad-version list.
- Trial (revised 2026-11-20): activation sets `trial=true`. ONLY `wallpaperVerified` (frames actually advancing after attach,
  the existing lifecycle evidence) calls `confirmHealthy()`; `aboutToQuit` no longer confirms, it calls `noteCleanExit()`
  (state `cleanExit`). Next start: trial + cleanExit => trial continues (not bad); trial without cleanExit (crash/hang) => rolled
  back and version marked bad. A failed/stalled application emits no `wallpaperVerified`, so it never confirms.
  `install()` while `trial=true` returns `TrialActive` (deferred, nothing written, ETag not saved so sync retries), so
  `previous.json` (last known-good) is never replaced by an unproven profile. Rollback restores that known-good profile.
  Limitation (by design, no expiry/retry counter): a profile whose trial keeps ending in clean exits without ever verifying stays
  in trial indefinitely; while it does, later profiles are deferred (`TrialActive`). A crash/hang in any trial run rolls back to the
  last known-good `previous.json` (or built-in defaults if none) and marks the version bad. Clean exit never confirms.
- Consent off: `record()` no-op, `ProfileSync::syncNow` returns before any network call. Turning it off
  discards unsent records. Profile download is also gated on opt-in.
- Endpoint comes only from env `MOTIVA_PERF_ENDPOINT` (dev). Empty => sync disabled. While the placeholder
  key is in use only loopback endpoints are accepted.

## Collected per record (coarse only)
hour-rounded time, kind, allowlisted extension, size bucket, resolution bucket (images, header read only),
stage (download/apply), ms, ok, `<stage>_failed`, profile version, gpu vendor, RAM GB, OS version, app version.
Never: URL, filename, path, content, user/machine name, install ID.

## Known limits
- Video codec/resolution/fps: collected passively via `VideoPlayer::videoMetadataAvailable` -> WallpaperManager -> `PerfProbe` (now stage `loaded`, see "Video event model"); no CPU/GPU or buffering metrics.
- No CPU/GPU utilisation or buffering metrics yet. Server-side aggregation/canary is out of repo.
- Only the three parameters above are tunable; no new processing strategies.
- `attachRetryMs`/`verifyWaitMs` apply to the next attach; the in-flight one keeps its value.

## Video event model (implemented 2026-11-20; unit-tested, NOT yet live-verified - see Verification below)
Four stages, all via `PerfProbe` (consent-gated, async). `meta` is legacy (accepted by the sanitiser, no longer emitted).
- `loaded`: video decoded far enough to read metadata. Fields: codec/fps/res buckets, `origin`
  (`startup_preview|user_selection|wallpaper_apply|playlist`, allowlisted). Independent of `applied`.
  It is first-seen video metadata, NOT one record per rotation/application: dedup uses media identity and ignores
  `origin`, so the same video loaded again (any origin) adds no record.
- `applied`: `wallpaperVerified` confirmed for the pending application. Metadata missing => codec/fps/res `unknown`.
  It is verified wallpaper application and currently carries NO `origin` field (origin exists only on `loaded`).
- `failed`: terminal failure: `load_failed` (loadFile false) or `player_error` (VideoPlayer error while pending).
- `cancelled`: `superseded` (newer setWallpaper), `superseded_by_load` (player holds a newer load at resolve time),
  `removed` (removeWallpaper / external wallpaper change), `shutdown`.
Correlation (all in memory, never serialized): `VideoPlayer::loadSeq()` bumps on every `loadFile`; metadata signal carries it.
`VideoApplyTelemetry::begin()` returns an `applyToken` (one pending at most); `bindLoad(token, loadSeq)`; the verify
lambda captures the token at schedule time, so a stale verification (older token, older attach generation) cannot resolve a
newer application. Metadata is matched by the token's `loadSeq`, not "latest". Each token resolves exactly once.
Errors are stamped: `VideoPlayer::loadError(loadSeq,msg)` (UI `errorOccurred` unchanged). `failedForLoad(token, errSeq)` fails only the
pending token whose bound load == errSeq; older-load errors are ignored (not failed, not cancelled). `setWallpaper` binds `loadSeq()+1`
before `loadFile` so synchronous errors match strictly.
Pending is bounded: one slot; STALLED retries keep it pending for at most 120 s (`kPendingLimitMs`, then `cancelled/attach_timeout`).
Wiring (reviewed 2026-11-20): `setWallpaper` starts one `QTimer::singleShot(kPendingLimitMs, this, ...)` per video application, capturing its
`applyToken`; it is never stopped (cannot be, singleShot) and does not need to be: `expire(token)` is a no-op unless that token is still the
pending one, and the timer dies with the manager. The 120 s constant is not injectable; only the token-gated `expire` is unit-tested
(`vt_boundedPendingCancelsNotFails`), the timer itself is not;
battery-hidden (at start or suspend) => `cancelled/battery_hidden` immediately (temporary platform unavailability, never `failed`);
removal/shutdown/supersession => cancelled. Resume from battery or a verify after cancellation produces no `applied` (token gone).
Event ordering: `loaded` is emitted when metadata arrives (first-seen, deduped, independent of any application). `applied` takes
metadata only for ITS bound loadSeq at resolve time; if absent => codec/fps/res `unknown`. Late metadata only emits `loaded`; the earlier
`applied` is never rewritten or duplicated; metadata of a superseded load never attaches to a newer application.
Telemetry queue: `perf-records.jsonl` lines are `<id>TAB<json>` (monotonic id, legacy bare-JSON lines are numbered and persisted on
first read). Uploaded JSON is unchanged (no id field). `peekBatchIds` + `acknowledgeIds(ids)` remove exactly those records; ids already
trimmed are ignored. `acknowledge(count)` was removed.
Dedup: `loaded` de-duplicated like other records (same media identity, origin not part of it); `applied/failed/cancelled`
set `alwaysRecord`, so identical repeated applications each produce a record. Only video gets tokens
(`VideoPlayer::isVideoSource`); images/GIF/procedural keep the legacy browser `apply` record.
Browser: `MainWindow::recordPerf` skips stage `apply` for video (no duplicate with `applied`); `download` unchanged.
Startup preview (`completeStartup`) and user selection emit `loaded` only. Playlist rotation while a wallpaper is
active reloads the shared player (`loaded`, origin `playlist`) with no new attach, so it yields no `applied`.

## Baseline (before implementation, from `%TEMP%\Motiva.log`, 16 browser applies, image requests, apply start -> confirmed)
median 1027 ms, mean 1102 ms, min 926, max 1870 (includes the fixed 700 ms verify wait).

## Verification (2026-11-19, scratch build in `%TEMP%\mp\appb`, executed)
Verified by running: apply via real `MotivaBrowserHost.exe` (native-messaging framing); 8 images (PNG/JPG/WebP/BMP,
800x600-4K) with opt-in off (0 records) and on (16 records, no path/URL/filename/user data); mock backend upload of
all records; valid v1/v2 profile activation (trial, cleared after first verified apply); crash-in-trial rollback
(marked bad); 7 invalid profiles all rejected; dead endpoint = applies unaffected; remote image URLs apply, truncated /
refused / 404 / non-image fail cleanly. Tests `PerfIntelligenceTests`, `DesktopScalingTests`, `BinaryRainTests`,
`VdDesktopTests` exit 0 (last saved totals 12/11/26/14 passed).
Timing, 12 equal applies, apply start -> confirmed: deployed build median 1277 ms, scratch off 1305, scratch on 1346
(no measurable regression; the 1027 ms baseline was taken under lighter load and is not comparable).
Gotchas: single-instance IPC means a running Motiva receives the host's requests, not the scratch exe; the scratch
exe needs Qt/MinGW on PATH; QSettings keys are `app\browserExtensionEnabled` and `intelligence\optIn` (subkeys).
Video hook (verified 2026-11-19): unit tests 13/13 (VideoPlayerRainTests, PerfIntelligenceTests); live: H.264 720p30 -> codec h264/fps <=30/res <=720p, MPEG-4 480p24 -> mpeg4/<=24; opt-off writes nothing; video apply ~1.0-1.3 s on and off. 
WebM/VP9 (investigated 2026-11-19; corrected 2026-11-20): The default FFmpeg backend directly decoded VP8/VP9 WebM in the harness. Its advertised extension list does not include WebM, so Motiva's extension gate (VideoPlayer::supportedVideoExtensions) rejects it. The Windows backend accepts WebM but returned unknown metadata in the tested environment. Actual on-screen rendering through the default backend after bypassing the gate is unverified. Truncated mp4 fails cleanly, no record. `loaded` records are bucketed (fps/res), exact values only in unit tests. Live runs: `--set-background <clip>` on scratch exe `%TEMP%\mpppb`; registry (HKCU\Software\Motiva\Motiva) backed up/restored; opt-in via `intelligence\optIn`.
Live verification of the 4-stage video model (2026-11-20, scratch exe `%TEMP%\mp\appb`, registry + data dir backed up and restored; executed):
startup preview -> one `loaded` (`startup_preview`), no `applied`; H.264 apply -> `loaded` (`wallpaper_apply`) + `applied` (~1.15 s);
same video 3x -> `loaded` once (dedup), `applied` x3; different video -> own codec/fps (mpeg4/<=30), no inherited metadata;
truncated mp4 -> one `failed`, reason `player_error` (synchronous load error fires before `bindLoad`, so reason is `player_error`, not `load_failed`), no `applied`;
WebM -> rejected by the extension gate, no record; browser video via real `MotivaBrowserHost.exe` + loopback http -> `download` + one `applied`, no `apply`;
opt-out -> 0 new records (startup, IPC apply, browser apply); new records contain only allowlisted keys, no path/URL/filename.
NOT verified: playlist rotation live (code review + unit tests only), Settings toggle clicked in the GUI, real Chrome extension, remote (non-loopback) video, interrupted network on video.
Gotcha: Motiva has no data/settings override; live runs must back up `HKCU\Software\Motiva`, the Run key and `%LOCALAPPDATA%\Motiva\Motiva` first. The HKCU Run entry already pointed at the scratch exe before this session.
Tests 2026-11-20: PerfIntelligenceTests 25/25, VideoPlayerRainTests 16/16 (lifecycle via `VideoApplyTelemetry` with a fake sink; WallpaperManager wiring itself is not unit-tested, needs a desktop).

Tests 2026-11-20 (executed, scratch build outside the repo): PerfIntelligenceTests 38/38, VideoPlayerRainTests 17/17. New: stale error, bounded pending,
loaded/applied ordering x3, queue trim/ack/concurrent append/legacy migration, trial deferral/quit/verified/recovery. WallpaperManager/MainWindow
wiring compiled syntax-only (g++ -fsyntax-only), not linked or run. Not covered: the 120 s timer itself in WallpaperManager (needs a desktop; not injectable).
2026-11-20 (later): PerfIntelligenceTests 39/39 (scratch build `%TEMP%\vt_tests_build`). Added `sync_mockServerFailUploadKeepsRecordsQueued`: mock `--fail-upload`
=> HTTP 500, summary "upload failed", record stays queued, mock records file never written, profile poll still activates v1.
2026-11-21 fixes (from code audit): `ProfileSync` rate limits allow a slack (min(300 s, period/20)) so the 6 h timer tick is not skipped
(cadence was ~12 h upload / ~30 h poll); failed downloads (no file) now record `download`/`download_failed` with no format/size;
`VideoApplyTelemetry` skips the file stat on the apply path unless consent is on (or a test sink is injected).

## 2026-11-21 gap closure (executed: scratch test build in %TEMP%\mt_build, out/build incremental build OK)
- `VideoPlayer`: errors delivered by QMediaPlayer signals go through `emitPlayerError`. `loadFile` posts a queued fence
  event (`m_fenceSeq`); an error delivered before the current load's fence (queued by an earlier load) is stamped stale,
  logged, and NOT emitted on `errorOccurred`. Confirmed defect at VideoPlayer level (tests fail without the fix) given a
  queued error; whether Qt's real backends queue errors that way is not live-verified. Stale metadata/EndOfMedia signals
  are NOT guarded (same class, out of scope, unchanged).
- `BrowserApplyTracker` (Core-only, unit-tested) now holds the browser-apply rules used by `MainWindow`: verification,
  battery-hidden and playback ERRORS affect the pending request only while its own file is `m_selectedVideoPath`
  (before, any non-"Could not attach" error failed it). Unsupported-file rejection fails it via `rejectRequestedFile`.
  Limitation: a failure of a different, newer load leaves the request to the 45 s timeout.
- `ProfileSync::due/kUploadPeriodSecs/kFirstSyncDelayMs/setClock`; `last==0` is always due. `PerfProbe::failedStageEvent`
  builds the failed-download record. `VideoApplyTelemetry(sink, statFn)` makes the consent-gated stat observable.
- Trial recovery policy is documented in the `ProfileStore.h` header comment. New: state.json `trialVersion` is persisted
  BEFORE active.json is replaced; an interrupted install is not a trial (nothing rolled back/marked bad). Forced kill before
  verification == crash: rollback + bad (by policy, indistinguishable). Legacy state without `trialVersion` behaves as before.
- Tests: PerfIntelligenceTests 56/56, VideoPlayerRainTests 20/20, BinaryRainTests 26, DesktopScalingTests 11, VdDesktopTests 14.
  VdWindowTests (needs a real desktop) not run.

## 2026-11-23 pre-live-testing safety fixes (verified: scratch builds in the session scratchpad; no live run)
- F4 `ProfileStore`: every write is checked (`Writer` seam `setWriterForTest`). `install()` returns `StoreFailed` (+`lastError()`)
  if the rollback copy, trial record or active.json cannot be written; known-good stays active, no trial starts. Rollback copy
  is an atomic replace (no remove-then-copy). `rollback()` keeps the trial record if active.json could not be restored (next start
  retries). `confirmHealthy()`/`noteCleanExit()` return bool; a failed confirm keeps the trial pending. `init()` does not continue a
  clean-exit trial whose marker it cannot clear (rolls back instead).
- F6 `ProfileStore::generation()` changes on init/install/rollback/reset (not setHardware). `WallpaperManager` captures it when a
  verification BEGINS and exposes `verifiedProfileGeneration()`; `MainWindow` calls `confirmHealthy(gen)` so a verification that
  started under an older profile never confirms a newer trial. The no-arg `confirmHealthy()` remains for tests only.
- F3 `BrowserApplyTracker::supersedeUnlessActive/cancel`: `MainWindow::loadVideoSource` (manual, drag&drop, playlist advance, any
  newer selection), `wallpaperRemoved` and `onRemoveVideo` answer a pending request once with a "Cancelled: ..." (ok=false)
  message and stop its timer; the request's own load (same path) is untouched; the 45 s timeout still covers genuinely pending ones.
- F2 `VideoPlayer::m_syncCallDepth` (was `m_inLoadFile`) also covers play/pause/setLoops/setPosition: an error raised synchronously
  inside our own call belongs to the current load. F1 limitation stays: Qt 6.12 `errorOccurred` has no load identity and the FFmpeg
  backend loads asynchronously (`setMediaAsync` + `CancelToken`, private header only; sources not available), so attribution of
  event-loop-delivered errors is by ORDER ONLY (best effort). Live test needed: apply a slow/invalid network video, immediately
  apply another file, and check the log for `Ignoring a player error queued before load` vs a wrong failure of the second request.
- Reviewed, unchanged: duplicate load in the browser path (same file, loadSeq N then N+1; only a perf cost); `runAfterStartup`
  wait is not covered by the 45 s timer (extension's 2 min idle timeout bounds it; startup always flushes the queue).
- Review fix: after a failed rollback `m_rollbackPending` blocks `confirmHealthy`/`noteCleanExit` from ending the trial; they retry the
  restore of active.json instead (`finishPendingRestore`). A stale previous.json that cannot be removed fails install (`StoreFailed`).
- Tests: PerfIntelligenceTests 77/77 (f3_*, f4_*, f6_*), VideoPlayerRainTests 22/22. Full app builds (scratch dir, 70 targets).
