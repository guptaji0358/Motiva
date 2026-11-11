# CLAUDE.md — Motiva entry point

This file is a navigation pointer only. Project knowledge lives in the memory system.

- **Memory directory:** `.claude/memory/`
- **Memory index (start here):** [.claude/memory/index.md](.claude/memory/index.md)

## Working rules

1. Before inspecting source code, read this file, then [index.md](.claude/memory/index.md), then only the memory files the task needs (the index maps tasks to files).
2. Then inspect the source files those notes name. Source code is the final authority; memory can be stale.
3. After any change to code, structure, dependencies, build, or behaviour, update the affected memory file(s) in the same task, and the index if files change.
4. Keep every memory Markdown file at 200 lines or fewer. Mark verified facts versus unverified ones; do not record planned work as implemented.
5. Do not treat `.claude/memory/history/` as current architecture; it is an archive of dated status logs.
