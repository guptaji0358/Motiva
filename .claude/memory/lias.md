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
- Task automations (2026-11-13, hand-authored manifests; tested: build failure path, risk denial): `build-compile` (risk 1; exit code + all errors), `memory-lias-update` (risk 1; append to a memory .md with 200-line guard, reindex, doctor), `git-commit-push --args {"message":..}` and `pr-create --args {"title":..,"body":..}` (risk 2: need `--allow-risk 2`; never force-push; refuse secret-looking files). git/PR success paths not yet exercised live.
- More task automations (2026-11-13): `deploy-local` (build + copy exe to deployment), `branch-create --args {"name":..}`, `precommit-check` (memory limit, debug markers, build), `installer-build` (runs installer/build-installer.ps1; NOT yet run live), `pr-checks` (risk 0; gh timed out on network in the test, so success path unverified). Tested live: precommit-check, deploy-local, branch-create.
- `code-outline` (2026-11-13, risk 0, tested live on extension/content.js: 62 matched lines instead of 420): `automation run code-outline --args {"file":"<rel path>","pattern":"<optional regex>"}` gives a line-numbered outline; read a whole file only if needed.
