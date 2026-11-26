# Development workflow

## Build (dev)

```bash
cmake -S . -B out/build -G Ninja -DCMAKE_PREFIX_PATH="C:/Qt/6.12.0/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build out/build
```

- Use the Qt-bundled MinGW (`C:\Qt\Tools\mingw1310_64\bin`), put it first on PATH.
- LIAS automations find Qt through `_common.py` (`qt_root`/`qt_env`): env `MOTIVA_QT_ROOT` or `QT_ROOT`, else
  `C:\Qt`, else `D:\Apps\Qt6`; they add MinGW, the kit, CMake and Ninja to PATH. Do not hardcode Qt paths.
- After moving the project folder, `out/build` must be reconfigured (`cmake --fresh ...`). Done 2026-11-26
  (Release, clean rebuild plus `--target MotivaLauncher`, both succeeded; not deployed or launched).
- `build-and-deploy.bat` does: `taskkill /F /IM Motiva.exe`, `cmake --build out/build`,
  copies `build\Motiva.exe` to `deployment\resources\bin\`.
- Kill a running `Motiva.exe` before rebuilding; the running exe locks the
  output. Git Bash form: `taskkill //F //IM Motiva.exe`.
- Clean deployment: `scripts/deploy/Deploy.ps1` (needs Release build with
  `MotivaLauncher`); installer: `scripts/installer/build-installer.ps1` -> `out/release/MotivaSetup.exe`.

## Build caveats

- The repo lives under OneDrive. Sync locks on fresh `out/build/` files can make
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

No test framework exists. Verification has been:
- out-of-tree harnesses (QtTest-style, offscreen grab of pages/themes, COM
  harness with fake `IShellItemArray`, native-messaging stand-in in Python);
- live runs and log inspection (Explorer restart cycles, `taskkill /F`);
- `grab()`/`snapshot()` screenshots of GL pages.
Record in memory what was verified and what was not (see [[known-issues]]).

## Binary Rain unit tests (opt-in)

`scripts/tests/` is a standalone CMake project (Qt Core/Gui/Test only): `cmake -S tests -B <dir> -G Ninja
-DCMAKE_PREFIX_PATH=<Qt kit>`, build, run `BinaryRainTests.exe` (on Windows pass `-o <winpath>,txt`
to see output; stdout is empty under msys). Exes (`DesktopScalingTests`: 11 passed, Core/Test only; `VdDesktopTests`: 14; `VdWindowTests`: 5, GUI, needs a real desktop + OpenGL, links the app sources): `BinaryRainTests` (26 passed 2026-11-18: config, persistence via QSettings INI, engine, frame pool)
and `VideoPlayerRainTests` (16 passed: play/pause/unload/suspend/config/target size; needs Multimedia, run with
`QT_QPA_PLATFORM=offscreen`). Not covered: Settings UI, preview, desktop attach, real CPU/GPU cost.
Qt kit moved to `C:\Qt\6.12.0\mingw_64` (was `D:\Apps\Qt6`, no longer present).

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
