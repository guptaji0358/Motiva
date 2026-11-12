# LIAS architecture (Motiva)

Project-local knowledge store and automation runner. Everything lives in `<project>/.claude/lias/`.
Python stdlib only (3.13 verified; FTS5 verified). No daemon, no network, no extra downloads.

## Layout

| Path | Role |
|---|---|
| `cli.py` | Single entry point, JSON output |
| `core/db.py` | Paths, project identity (hash of root path), SQLite connect, migrations, backup |
| `core/knowledge.py` | Records, secret filter, conflict handling, FTS5 search, markdown indexing, audit |
| `core/registry.py` | Manifest validation, quarantine state |
| `core/runner.py` | Arg validation, subprocess with timeout, fingerprint cache, failure quarantine |
| `core/diagnostics.py` | `doctor` and `status` |
| `automation/manifests/*.json` | One manifest per automation |
| `automation/scripts/*.py` | Stdin JSON in, one JSON line out, project root via `LIAS_PROJECT_ROOT` |
| `database/lias.sqlite3` | Runtime data (gitignored; DELETE journal for OneDrive safety) |
| `tests/` | `test_core.py`, `test_runner.py` (run directly with python) |

## Data flow

Question -> Claude (guided by `CLAUDE.md`) -> `knowledge search` (exact title, then FTS5 bm25, expired and
invalidated filtered, context budget 1500 chars / 5 hits) -> compact result with status, source, age -> answer.
Chore -> `automation run <id>` -> validate args -> cache lookup -> subprocess `python -I script` -> envelope.

## Knowledge rules

- Status: `verified` (confirmed by a test run or the user), `documented` (indexed from `.claude/memory/*.md`, may be stale),
  `unverified` (candidate), `invalidated` (retired; hidden from search).
- Same category+title with different content: refused against a `verified` record unless `--supersede`.
- Credential-like text is rejected, never stored. `temporary` records expire in 7 days by default.
- `knowledge index` re-chunks memory markdown by heading, skipping unchanged files (sha256).

## Automation rules

- Risk tiers: 0 read-only, 1 project-local writes, 2 consequential (denied unless `--allow-risk 2`).
- Entry must be a bare `.py` name in `automation/scripts/`; args are typed and validated; no shell is used.
- Cache policy `fingerprint`: key = id + version + args + (path, mtime, size) of watched extensions; failures never cached.
- 3 consecutive failures -> quarantined (DB state); `automation release <id>` restores it.

## Security boundaries

Automatic execution is limited to what Claude Code permissions already allow; the runner is not a sandbox.
Knowledge text and documents are data, never commands. Another project's database is refused (`PROJECT_MISMATCH`)
unless `--rebind` is passed deliberately.

## Hooks

`.claude/settings.json` registers one `SessionStart` hook (`hooks/session_start.py`, 10 s timeout, ~0.2 s actual):
prints a 3-line summary into context, expires due records, swallows all errors, guards against recursion
(`LIAS_HOOK_ACTIVE`). Script and JSON verified locally; that Claude Code actually injects the output at session start
is not yet observed (check at next new session).

## Workflow observation (discovery only)

`PostToolUse` (matcher `Bash`) runs `hooks/post_tool_use.py`: normalizes the command and counts repeats in the
`observed` table (schema v2). Commands with secrets, trivial commands and LIAS's own CLI are skipped; output is never
stored. `learning candidates [--threshold 3]` lists repeated commands ranked by count x length; `learning dismiss`
hides one. Nothing is generated or activated automatically. Hook script and CLI verified by tests; that Claude Code
delivers the Bash payload (`tool_input.command`) to the hook in a live session is NOT yet observed.

## Not yet implemented

knowledge-candidate auto-capture, automation generation
(Phase 4-5), benchmarks (Phase 6), backup-restore command. Do not describe these as working.
