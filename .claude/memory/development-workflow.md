# Development workflow

## Build (dev)

```bash
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="D:/Apps/Qt6/6.12.0/mingw_64" -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

- Use the Qt-bundled MinGW (`D:\Apps\Qt6\Tools\mingw1310_64\bin`), put it first on PATH.
- `build-and-deploy.bat` does: `taskkill /F /IM Motiva.exe`, `cmake --build build`,
  copies `build\Motiva.exe` to `deployment\resources\bin\`.
- Kill a running `Motiva.exe` before rebuilding; the running exe locks the
  output. Git Bash form: `taskkill //F //IM Motiva.exe`.
- Clean deployment: `deploy/Deploy.ps1` (needs Release build with
  `MotivaLauncher`); installer: `installer/build-installer.ps1` -> `release/MotivaSetup.exe`.

## Build caveats

- The repo lives under OneDrive. Sync locks on fresh `build/` files can make
  windres/`cc1.exe` fail silently. Retry, or `rm -rf build` and reconfigure,
  before debugging source (see history log 04).
- Qt6OpenGL.dll and Qt6OpenGLWidgets.dll must be deployed (`resources/Qt/`).

## Run and observe

- Run `build/Motiva.exe` (or `deployment\...\Motiva.exe`).
- Read `%TEMP%\Motiva.log` first when diagnosing desktop attach. Startup lines
  include `[Discover]`, `[Shell]`, `[Verify]`, `[Hierarchy]`, `[IPC]`, `[Lifecycle]`.
- Second launch (`build/Motiva.exe` while running) triggers recovery via IPC,
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
