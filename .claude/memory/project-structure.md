# Project structure

Verified with `git ls-files` (306 tracked files) on 2026-11-11.
Source totals: about 34.8k lines across `scripts/src`, `scripts/include`, `scripts/main`.

## Top level

| Path | Responsibility |
|---|---|
| `CMakeLists.txt` | Build for `Motiva`, `MotivaShell`, `MotivaBrowserHost`, `MotivaLauncher` |
| `README.md` | User-facing readme (features, build, limitations) |
| `build-and-deploy.bat` | Dev helper: kills Motiva, builds `build/`, copies exe to `deployment\resources\bin` |
| `scripts/` | All C++ source (see below) |
| `resources/` | `app.rc` (icon/version), `app.qrc` (embedded Assets), `app.manifest` |
| `Assets/` | Feature-organized images/SVGs (see below) |
| `extension/` | Browser extension (MV3): `manifest.json`, `background.js`, `content.js`, `popup.*`, `icons/` |
| `third_party/rclone/` | Bundled rclone sources/binary notes (rclone.exe itself is gitignored) |
| `deploy/Deploy.ps1` | Builds the clean `deployment/` tree (launcher + `resources/bin` + Qt runtime + plugins) |
| `installer/` | `MotivaSetup.iss` (Inno Setup), `build-installer.ps1`, `make-art.ps1`, `assets/ui` |
| `docs/` | `CLOUD_STORAGE.md` (cloud design; describes REMOVED sync/backup features — stale), `readme/*.svg` |
| `website/` | Static marketing site (`index`-level HTML, `css/`, `js/`, `Assets/`) |
| `release/` | Gitignored output (`MotivaSetup.exe`) |
| `deployment/` | Gitignored output tree |
| `build/` | Gitignored CMake/Ninja build dir |
| `.claude/` | Claude Code config: `CLAUDE.md` (legacy full status log, see [[index]]), `memory/` (this system) |

## scripts/

- `scripts/main/` — `main.cpp` (the `Motiva` entry), `launcher.cpp` (deploy-only launcher).
- `scripts/include/` — ~45 headers (declarations only). Includes the GL UI toolkit
  (`GlUi.h`, `GlWindow.h`, `GlPopups.h`, `GlColor.h`), `Theme.h`, `DialogSizing.h`.
- `scripts/src/` — ~42 implementation files, one per class/feature.
- `scripts/shell/MotivaShell.cpp` — Explorer COM handler DLL (not in `src/`).
- `scripts/browser/MotivaBrowserHost.cpp` — native messaging host exe.
- `scripts/tools/` — `brand_rclone_exe.py` (replaces rclone icon/version strings),
  `patch_rclone_page.py` (patches rclone's sign-in page).
- `scripts/superseded-oauth-cloud/` — 8 files of the **old, unbuilt** hand-written
  OAuth cloud layer. Kept for reference only; not compiled.

Include path: `target_include_directories(Motiva PRIVATE scripts/include)`, so
sources use plain `#include "Foo.h"`.

Rule: only `main.cpp` contains `int main` in `Motiva`; `launcher.cpp` is a
separate exe.

## Assets/

Organised by feature, not by file type (`icons/`, `images/`, `gifs/` were
explicitly rejected). Do not add empty placeholder folders. Folders present:
`application/` (app icon `motiva.ico`, logo), `settings-icon/`, `video/`,
`wallpaper/{light,dark}`, `playlist/{light,dark}` (+ `playlist-add.svg`,
`video-badge.svg`), `flow/{light,dark}`, `category/{light,dark}` +
`category-folder.svg`, `status/`, `playback/`, `checkbox/`.
Qt resource aliases mirror the folder path, e.g. `:/application/motiva.ico`,
`:/playlist/playlist-add.svg`. Code must use `:/` paths, never disk paths.

## website/

Static site: `website/index.html`-level page plus `css/style.css` (currently
has an uncommitted 1-line change), `js/`, `Assets/{icons,screenshots,logo,
backgrounds,other,videos}`.

## Git state at snapshot time

Uncommitted: root `CLAUDE.md` deleted from the working tree (it is tracked in
HEAD; `.gitignore` also lists `CLAUDE.md`); modified `scripts/shell/MotivaShell.cpp`,
`scripts/src/WebMediaCache.cpp`, `website/css/style.css`. Git prints LF->CRLF
warnings for the two `.cpp` files.

Related: [[architecture]] · [[modules]] · [[technology-stack]]
