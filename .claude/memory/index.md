# Motiva memory index

Start here. Read the file(s) for the task, then the source it names. Source code
is the final authority; these notes say where to look. Last full pass: 2026-11-11.

## Memory files

| File | Read it when... |
|---|---|
| [overview.md](overview.md) | You need the product purpose, features, or maturity |
| [architecture.md](architecture.md) | You need build targets, layers, threading, process model, storage paths |
| [project-structure.md](project-structure.md) | You need to find where something lives (folders, Assets, git state) |
| [technology-stack.md](technology-stack.md) | You need versions, toolchain, Qt modules, rclone, extension manifest |
| [modules.md](modules.md) | You need the responsibility of a class before opening it |
| [data-flow.md](data-flow.md) | You are changing startup, attach, IPC, Explorer, browser, cloud or shutdown |
| [development-workflow.md](development-workflow.md) | You build, deploy, run, log, or test |
| [decisions.md](decisions.md) | You are about to change a design choice and need its reason |
| [known-issues.md](known-issues.md) | You need what is unverified, limited, or broken |
| [lias.md](lias.md) | You use or change the local knowledge/automation system in `.claude/lias/` |

## Task to file map

- Wallpaper does not show / Explorer restart / attach: [data-flow](data-flow.md) §2-3, [modules](modules.md) (WallpaperManager, WindowsDesktopWallpaper), [decisions](decisions.md) Desktop attach.
- Playlist order, rotation, categories, mixed: [modules](modules.md) Playlists, [data-flow](data-flow.md) §4.
- Explorer menu: [data-flow](data-flow.md) §5, [modules](modules.md) Shell & browser.
- Browser extension: [data-flow](data-flow.md) §6, `extension/`, [technology-stack](technology-stack.md).
- Cloud: [data-flow](data-flow.md) §7, [modules](modules.md) Backup/cleanup/cloud.
- UI or theme work: [architecture](architecture.md) layer 2, [modules](modules.md) UI toolkit, [development-workflow](development-workflow.md) editing rules.
- Build or installer: [development-workflow](development-workflow.md), [technology-stack](technology-stack.md) Packaging.

## Historical archive (not current architecture)

`history/status-log-01` to `status-log-11` are the original dated status
entries (2026-09-07 to 2026-10-25), moved verbatim from the former monolithic
`.claude/CLAUDE.md` (1563 lines). They use old paths (`src/`, `VideoWallpaper`)
and superseded claims. Use them only for the reasoning and verification history
they record; the topic files above are authoritative. Archive map:

- 01: Explorer COM menu, browser extension, rclone branding, cloud (read-only, rclone), GL cloud, folder menu v2.1, GL main window
- 02: GL dialogs/popups, Settings/Playlists GL, mixed playlists + nested categories, tabbed settings, notifications, Find All
- 03: startup robustness + SVG icons, Explorer menu/categories, per-playlist categories + backup, Cleanup & Reset
- 04: Assets layout, settings icon, drag & drop
- 05: "Set as wallpaper" interference (live case + root cause)
- 06: UI redesign, product name, "what this is", planned work, build
- 07: wallpaper visibility BLOCKED investigation, startup-delay elimination
- 08: persistent diagnostics + second-instance IPC
- 09: Explorer restart race fix, startup ~2 min fix
- 10: Explorer deadlock fix, DirectComposition rewrite, original and pre-DComp status
- 11: Assets folder, source layout, code map (superseded by [project-structure](project-structure.md))

## Maintenance

- Keep every file at or under 200 lines (target 100-170). Split by topic when near the limit.
- Update the affected file in the same task as any behaviour, structure or dependency change.
- Mark verified vs. unverified; never describe planned work as implemented.
- Add a new file only when a topic has no home; update this index when files change.
