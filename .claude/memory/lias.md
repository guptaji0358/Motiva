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

- Implemented and tested (23 unit tests): DB + migrations, knowledge store with FTS5, secret rejection,
  conflict protection, expiry, markdown indexing, runner (timeouts, arg validation, cache, quarantine), doctor/status.
- Automations: `source-stats` (fingerprint-cached; 821 ms cold, 9 ms cached on this repo), `build-env-check`.
- Not implemented: hooks, auto-capture, automation generation, benchmarks. `database/` is gitignored and local.
- After editing memory markdown, run `knowledge index`; `documented` records are derived from it.

## Gotchas

- Tests are run as files (`python .claude/lias/tests/test_core.py`), not via `unittest discover`.
- A copied/moved project raises `PROJECT_MISMATCH`; use `--rebind` only if intended.
- Automation ids need 2+ chars (`[a-z0-9-]`); invalid manifests are reported in `bad_manifests`, not fatal.
