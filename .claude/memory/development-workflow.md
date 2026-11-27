# Development workflow

## Build (dev)

```bash
cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build out/build
```

- Use the Qt-bundled MinGW (`C:\Qt\Tools\mingw1310_64\bin`), put it first on PATH.
- LIAS automations find Qt through `_common.py` (`qt_root`/`qt_env`): env `MOTIVA_QT_ROOT` or `QT_ROOT`, else
  `C:\Qt`; they add MinGW, the kit, CMake and Ninja to PATH. Do not hardcode Qt paths.
- After moving the project folder, `out/build` must be reconfigured (`cmake --fresh ...`). Done 2026-11-26
  (Release, clean rebuild plus `--target MotivaLauncher`, both succeeded; not deployed or launched).
- `build-and-deploy.bat` does: `taskkill /F /IM Motiva.exe`, `cmake --build out/build`,
  copies `build\Motiva.exe` to `deployment\resources\bin\`.
- Kill a running `Motiva.exe` before rebuilding; the running exe locks the
  output. Git Bash form: `taskkill //F //IM Motiva.exe`.
- Clean deployment: `scripts/deploy/Deploy.ps1` (needs Release build with
  `MotivaLauncher`); installer: `scripts/installer/build-installer.ps1` -> `out/release/MotivaSetup.exe`.
- `build-installer.ps1 [-BuildDir d] [-OutputDir d]` never kills processes (refuses, exit 2); non-default dirs deploy to
  `<OutputDir>\deployment`. The CMake `deploy` target itself has no process check and recreates `out/deployment`.
- `Deploy.ps1` fails on an incomplete tree (its list must follow Qt module changes) and skips `extension/tests`.
- `MotivaLauncher` must stay fully static; whatever starts the deployed app (browser host included) uses the root
  launcher, not `resources/bin/Motiva.exe`.

## Build caveats

- An earlier repo location was under OneDrive (the current `D:\Apps\...` path is outside the user's OneDrive
  folder, checked 2026-11-27). There, sync locks on fresh `out/build/` files can make
  windres/`cc1.exe` fail silently. Retry, or `rm -rf build` and reconfigure,
  before debugging source (see history log 04).
- Qt6OpenGL.dll and Qt6OpenGLWidgets.dll must be deployed (`resources/Qt/`).

## Run and observe

- Run `out/build/Motiva.exe` (or `deployment\...\Motiva.exe`).
- Read `%TEMP%\Motiva.log` first when diagnosing desktop attach. Startup lines
  include `[Discover]`, `[Shell]`, `[Verify]`, `[Hierarchy]`, `[IPC]`, `[Lifecycle]`.
- Second launch (`out/build/Motiva.exe` while running) triggers recovery via IPC,
  a handy way to re-attach without UI automation.
- Test processes launched from the Claude desktop session are MSIX-virtualised:
  `%APPDATA%`/HKCU writes land in the package overlay. To test real storage,
  launch outside the package through WMI (`Win32_Process.Create`, with
  `ShowWindow=1`).

## Verification style used in this repo

The app build runs no tests; opt-in tests are listed below. Other verification has been:
- out-of-tree harnesses (QtTest-style, offscreen grab of pages/themes, COM
  harness with fake `IShellItemArray`, native-messaging stand-in in Python);
- live runs and log inspection (Explorer restart cycles, `taskkill /F`);
- `grab()`/`grabFramebuffer()` screenshots of GL pages.
Record in memory what was verified and what was not (see [[known-issues]]).

## Opt-in tests

`scripts/tests/` is a standalone CMake project: `cmake -S scripts/tests -B <dir> -G Ninja
-DCMAKE_PREFIX_PATH=<Qt kit>`, build, run each exe (on Windows pass `-o <winpath>,txt` to see output; stdout
is empty under msys). Latest recorded run (2026-11-27 after dead-code removal, LIAS `motiva-regression`, [[lias]]):
190 passed, 0 failed, 0 skipped; `VdWindowTests` not run (its count below is from 2026-11-26).

| Exe | Passed | Notes |
|---|---|---|
| `BinaryRainTests` | 26 | config, QSettings INI persistence, engine, frame pool |
| `WebMediaNameTests` | 29 | web-cache file names (added 2026-11-27) |
| `DesktopScalingTests` | 11 | Core/Test only |
| `VdDesktopTests` | 14 | pure Virtual Desktop logic |
| `VideoPlayerRainTests` | 22 | needs Multimedia; `QT_QPA_PLATFORM=offscreen` |
| `PerfIntelligenceTests` | 88 | needs Network, Python + `cryptography` |
| `VdWindowTests` | 5 | GUI: real desktop + OpenGL, links app sources |

Not covered: Settings UI, preview UI, desktop attach, real CPU/GPU cost. Browser extension:
`node --test scripts/extension/tests`. Qt kit is `C:\Qt\6.12.0\mingw_64` (`D:\Apps\Qt6` is gone).

## Do not

- Do not inject keystrokes or synthetic input to test shortcuts; the user's PC is
  in use (CLAUDE history, 2026-10-05).
- Do not run a tight loop of Explorer restarts; space them and wait for the
  previous `TaskbarCreated` log line.
- Do not reintroduce the 0x052C spawn-message loop on the hot path (see [[decisions]]).

## Editing rules for this repo (from the history)

- Use the Assets feature folders; never add placeholder folders.
- Theme and colors come from `Theme`/tokens; do not hard-code colours in new UI.
- GL UI: a delegate must clip with `Qt::IntersectClip`; `GlItemView`
  clears hidden rows on structural change, so the owner re-applies its filter.
- `slots` is a Qt macro; do not use it as a variable name.
- `open()` in the library must `finish()` every query before a migration.
- Log writes must seek to EOF before each write (two processes share the log).

## Memory maintenance

Update the relevant file under `.claude/memory/` in the same task as any
code change that alters behaviour, layout, or a verified/unverified status.
Keep every file under 200 lines; see [[index]].
