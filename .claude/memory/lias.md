# LIAS (project-local knowledge and automation)

Verified 2026-11-12 by running the code and tests. Full design: `.claude/lias/docs/architecture.md`.

## Use

```bash
python .claude/lias/cli.py knowledge search "question"
python .claude/lias/cli.py automation list
python .claude/lias/cli.py automation run source-stats
python .claude/lias/cli.py automation run build-env-check
python .claude/lias/cli.py doctor
python .claude/lias/cli.py knowledge index   # re-index memory/*.md after editing them
```

## State

- Implemented and tested (see test counts below): DB + migrations, knowledge store with FTS5, secret rejection,
  conflict protection, expiry, markdown indexing, runner (timeouts, arg validation, cache, quarantine), doctor/status.
- Automations: `source-stats` (fingerprint-cached; 821 ms cold, 9 ms cached on this repo), `build-env-check`.
- SessionStart hook (`.claude/settings.json` -> `lias/hooks/session_start.py`): script verified; injection into a live session NOT yet observed.
- PostToolUse(Bash) hook + `learning candidates`: counts repeated commands (43 tests: core 12, runner 11, observer 5, generation 10, maintenance 4, e2e 1). Live delivery CONFIRMED 2026-11-12 (this session's Bash commands appeared in `observed`).
- Generation pipeline: `automation propose|promote|retire <id>` (see docs/architecture.md); `memory-line-check` was created this way.
- `backup create|list|restore`, `benchmark` (local timings + char proxy; tokens not measured; see architecture.md).
- `tests/test_e2e.py`: two real project copies driven via CLI in separate processes (persistence, isolation, generate+promote+reuse, risk denial, quarantine, backup/restore). Passed 2026-11-13.
- Deliberately not built: auto-capture (no success signal in the hook payload; facts are saved only via `knowledge add --status verified`) and unprompted generation (Claude authors candidates; LIAS validates them).
- Still unconfirmed: SessionStart output injection in a fresh session. `database/` is gitignored and local.
- After editing memory markdown, run `knowledge index`; `documented` records are derived from it.

## Gotchas

- Tests are run as files (`python .claude/lias/tests/test_core.py`), not via `unittest discover`.
- A copied/moved project raises `PROJECT_MISMATCH`; use `--rebind` only if intended.
- Automation ids need 2+ chars (`[a-z0-9-]`); invalid manifests are reported in `bad_manifests`, not fatal.
