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
- `.claude/lias/tests/test_e2e.py`: two real project copies driven via CLI in separate processes (persistence, isolation, generate+promote+reuse, risk denial, quarantine, backup/restore). Passed 2026-11-13.
- Deliberately not built: auto-capture (no success signal in the hook payload; facts are saved only via `knowledge add --status verified`) and unprompted generation (Claude authors candidates; LIAS validates them).
- Still unconfirmed: SessionStart output injection in a fresh session. `database/` is gitignored and local.
- After editing memory markdown, run `knowledge index`; `documented` records are derived from it.

## Autonomous engineering agent (2026-11-26)

- `python .claude/lias/cli.py agent ...` (code `.claude/lias/agent/`, full doc `.claude/lias/docs/agent.md`). Deterministic worker: JSON task or workflow in, Markdown report out; grants/approvals per task. Data in gitignored `lias/agent-data/`. Does not need the LIAS database except for `lias_automation` steps.
- Motiva check: `... agent do --workflow motiva-regression --grant recovery [--grant visible-desktop] --scratch <dir>` (VdWindowTests opens real windows, hence the grant). Defaults to the `C:\Qt` kit; never touches `out/`, never kills processes.
- Latest run 2026-11-27: seven suites in the workflow; six ran, 190 passed, 0 failed, 0 skipped (26/29/11/14/22/88);
  `VdWindowTests` blocked (no `visible-desktop` grant). Git and `HKCU\Software\Motiva` unchanged; unchanged rerun
  reused all six. 2026-11-26: real missing-DLL recovery shown. Edit action, approvals, interruption recovery: unit tests only.
- Tests: `tests/test_agent.py`, `tests/test_agent_safety.py` (fixtures `tests/agent_fixtures.py`).
- 2026-11-27: the report now states grants, approvals (no approver identity is stored), execution history and per-check
  verification (agent.md "Report"); attempts record their run number. test_agent 26 and test_agent_safety 29 pass.
- Fixed 2026-11-27: the `sk`/`pk` secret pattern (`core/knowledge.py`) redacted the saved state `skipped_dependency`, so a dependent of a blocked step never resumed. Prefixes now need `-`/`_` (not AKIA/AIza). Verified: test_agent 26, test_agent_safety 25, test_core 12, test_observer 5 pass; live temp-dir smoke task completed after approval. Tasks saved before the fix with `[REDACTED]` states stay stuck; submit anew.
- 2026-11-26: the database was bound to the old project path; rebound to this folder with the owner's approval (`--rebind`; copy kept as `backups/lias-pre-rebind-*.sqlite3`).
- 2026-11-27 build tooling: Qt comes from `MOTIVA_QT_ROOT`/`QT_ROOT`, else `C:\Qt`. `build-compile` no longer kills
  Motiva; with `precommit-check` it uses `_build_guard.py` (optional `build_dir`; refuses an unconfigured/foreign dir or
  a process running from it; never configures). `installer-build` takes `build_dir`/`output_dir`; a non-default value
  leaves `out/deployment` alone. Versions 1.1.0 (build-env-check 1.1.1, deploy-local 1.2.1). `build-compile` was
  quarantined by three `cmake` not-found runs (fixed), then released with `automation release`. All three build
  checks and `installer-build` passed in scratch dirs; default `out/` paths not run. Tests: `test_build_guard.py` (8).

## Gotchas

- Tests are run as files (`python .claude/lias/tests/test_core.py`), not via `unittest discover`.
- A copied/moved project raises `PROJECT_MISMATCH`; use `--rebind` only if intended.
- Automation ids need 2+ chars (`[a-z0-9-]`); invalid manifests are reported in `bad_manifests`, not fatal.
- Task automations (2026-11-13, hand-authored manifests; tested: build failure path, risk denial): `build-compile` (risk 1; exit code + all errors), `memory-lias-update` (risk 1; append to a memory .md with 200-line guard, reindex, doctor), `git-commit-push --args {"message":..}` and `pr-create --args {"title":..,"body":..}` (risk 2: need `--allow-risk 2`; never force-push; refuse secret-looking files). git/PR success paths not yet exercised live.
- More task automations (2026-11-13): `deploy-local` (details in the next bullet), `branch-create --args {"name":..}`, `precommit-check` (memory limit, debug markers, build), `installer-build` (see above), `pr-checks` (risk 0; gh timed out on network in the test, so success path unverified). Tested live (2026-11-13): precommit-check, branch-create, and the pre-1.1 `deploy-local`.
- `deploy-local` 1.2.1 executed 2026-11-27 only on a temporary project copy (dry run, build + deploy, skip_build fresh/refused; a `cmake` lookup bug was fixed); on the real tree only a dry run, which refused because Motiva runs from `out/`.
- `deploy-local` design: inputs `dry_run`, `qt_root`, `skip_build`, `allow_stale_artifacts` (strict JSON types). Qt kit/MinGW come from `out/build/CMakeCache.txt` (never reconfigured) and must lie under `qt_root`/`MOTIVA_QT_ROOT`/`C:\Qt` with numeric-version `mingw_64` + `mingwNNNN_64` names; mismatch = refusal, never a silent kit switch. `skip_build` (freshness rewritten 2026-11-21 in `_deploy_fresh.py`, tested by `tests/test_deploy_freshness.py` (17 tests, passed), not run in a real deployment) judges EACH exe against its own inputs, taken read-only from `out/build/build.ninja` (never runs ninja: `ninja -t` deleted an old-format `.ninja_log`): the target's transitive explicit/implicit inputs (order-only links never reach another exe), plus the project source/-I directories of that target (headers), plus shared config for all targets (root CMakeLists, CMakeCache, build.ninja and project-owned CMake inputs). A Motiva-only change leaves the host fresh and vice versa; a regenerated build.ninja/CMakeLists change makes both stale (no per-target config signature exists). Unreadable/odd build.ninja, no rule for an exe, missing input or no project source = not fresh with a reason; otherwise refused unless `allow_stale_artifacts` (limit: deleted project sources are not detected; header deps come from directory sweeps, not `.ninja_deps`). Refuses if Motiva/MotivaBrowserHost run from `out/` (checked before and after the build; nothing is killed) or if a `.deploy-stage-*` dir is left over (reported, never deleted). Copies the two exes + `scripts/extension/` (skips `tests`) into `out/deployment/resources/bin`; stale extension files already in the destination are left in place (merge, not mirror); launcher, Qt DLLs, settings untouched. Apply = stage + SHA-256 verify + backup, then per-file `os.replace`; failure rolls back overwritten files and removes only files/dirs this run created. NOT atomic as a set: a kill/power loss during apply, or a failed rollback, can leave a mixed deployment (stage dir with `journal.json` + backups is kept). Build timeout 540 s stops only `cmake` (output goes to a temp file so the timeout returns promptly); Ninja/compiler descendants may keep running (no process-tree kill: needs a Job Object created before spawn, not validated, so not attempted).
- `code-outline` (2026-11-13, risk 0, tested live on scripts/extension/content.js: 62 matched lines instead of 420): `automation run code-outline --args {"file":"<rel path>","pattern":"<optional regex>"}` gives a line-numbered outline; read a whole file only if needed.
- `claude-subagent-manager` (2026-11-23, risk 1, tested: `tests/test_subagent_manager.py`, 11 tests): generates native Claude Code sub-agent files `.claude/agents/<name>.md` from a spec/team JSON (`--args {"action":"validate|create|team|list","team_file":"<rel>","spec_file":"<rel>","dry_run":bool}`). Never overwrites (whole team refused if any file exists), roles read-only/verifier/implementation, forbids Agent/Task tools, rejects unsafe paths. Motiva team: `.claude/lias/agent-teams/motiva-team.json` (5 agents). New agent files are only loadable as `subagent_type` after a Claude Code restart; before that run them headless: `claude -p --agent <name> --allowedTools "Read Grep Glob" < prompt.txt`. Headless agents did not pin the Qt MinGW compiler (used MSYS2): always pass -DCMAKE_CXX_COMPILER explicitly.
