# Project structure

Verified with `git ls-files` (457 tracked files, HEAD `8806853`) on 2026-11-27.
Source totals: about 39.5k lines across `scripts/src`, `scripts/include`, `scripts/main`.

## Top level

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Build for `Motiva`, `MotivaShell`, `MotivaBrowserHost`, `MotivaLauncher` |
| `README.md` | User-facing readme (features, build, limitations) |
| `build-and-deploy.bat` | Dev helper: force-kills Motiva, builds `out/build/`, copies exe to `out/deployment/resources/bin`, launches it |
| `scripts/` | All C++ source (see below) |
| `scripts/resources/` | `app.rc` (icon/version), `app.qrc` (embedded Assets), `app.manifest` |
| `Assets/` | Feature-organized images/SVGs (see below) |
| `scripts/extension/` | Browser extension (MV3): `manifest.json`, `background.js`, `content.js`, `popup.*`, `icons/` |
| `third_party/rclone/` | Bundled rclone sources/binary notes (rclone.exe itself is gitignored) |
| `scripts/deploy/Deploy.ps1` | Builds the clean `out/deployment/` tree (launcher + `resources/bin` + Qt runtime + plugins) |
| `scripts/installer/` | `MotivaSetup.iss` (Inno Setup), `build-installer.ps1`, `make-art.ps1`, `assets/ui` |
| `docs/` | `CLOUD_STORAGE.md` (read-only rclone cloud), `readme/*.svg` |
| `scripts/website/` | Static marketing site (`index`-level HTML, `css/`, `js/`, `Assets/`) |
| `out/release/` | Gitignored output (`MotivaSetup.exe`) |
| `out/deployment/` | Gitignored output tree |
| `out/build/` | Gitignored CMake/Ninja build dir |
| `.claude/` | Claude Code config: `CLAUDE.md` (short pointer), `memory/` (this system), `lias/` ([[lias]]), `agents/` |

## scripts/

- `scripts/main/` — `main.cpp` (the `Motiva` entry), `launcher.cpp` (deploy-only launcher).
- `scripts/include/` — ~60 headers (declarations only). Includes the GL UI toolkit
  (`GlUi.h`, `GlWindow.h`, `GlPopups.h`, `GlColor.h`), `Theme.h`, `DialogSizing.h`.
- `scripts/src/` — ~55 implementation files, one per class/feature.
- `scripts/shell/MotivaShell.cpp` — Explorer COM handler DLL (not in `src/`).
- `scripts/browser/MotivaBrowserHost.cpp` — native messaging host exe.
- `scripts/tools/` — `brand_rclone_exe.py` (replaces rclone icon/version strings),
  `patch_rclone_page.py` (patches rclone's sign-in page), `mock-perf/` (mock backend; its dev key is gitignored).
- `scripts/tests/` — opt-in standalone CMake test project ([[development-workflow]]).
- The old hand-written OAuth cloud layer (`scripts/superseded-oauth-cloud/`) was deleted on
  2026-11-27 with other leftovers; it is in git history up to `8806853`.

Include path: `target_include_directories(Motiva PRIVATE scripts/include)`, so
sources use plain `#include "Foo.h"`.

Rule: only `main.cpp` contains `int main` in `Motiva`; `launcher.cpp` is a
separate exe.

## Assets/

Organised by feature, not by file type (`icons/`, `images/`, `gifs/` were
explicitly rejected). Do not add empty placeholder folders. Folders present:
`application/` (`motiva.ico`; `motiva_256.png` feeds `make-art.ps1`), `settings-icon/`, `video/`,
`wallpaper/{light,dark}`, `playlist/{light,dark}` (+ shared icons such as
`video-badge.svg`), `flow/{light,dark}`, `category/{light,dark}`, `status/`, `playback/`, `checkbox/`.
Qt resource aliases mirror the folder path, e.g. `:/application/motiva.ico`,
`:/playlist/video-badge.svg`. Code must use `:/` paths, never disk paths.

## scripts/website/

Static site: `scripts/website/index.html`-level page plus `css/style.css`, `js/`,
`Assets/{icons,screenshots,logo,backgrounds,other,videos}`.

## Git notes

Root `CLAUDE.md` is tracked although `.gitignore` also lists `CLAUDE.md`. Check `git status` for the
current working-tree state; it is not recorded here.

Related: [[architecture]] · [[modules]] · [[technology-stack]]
