# LIAS Autonomous Engineering Agent (`.claude/lias/agent/`)

A deterministic worker: Claude (or a person) owns the task; Python plans, executes, verifies, recovers from
recognised failures and writes a report. No LLM, no cloud service, standard library only.

## Relationship

```
task owner --JSON spec / workflow--> cli.py agent --> planner -> engine -> actions
task owner <--Markdown report------- report.py <---- verify / diagnose / store
```

## Commands (`python .claude/lias/cli.py agent ...`)

| command | purpose |
|---|---|
| `do <spec.json>` / `do --workflow NAME` | submit, run, print the report |
| `submit ...`, then `plan <id>` | inspect waves, permissions and digests before running |
| `run <id>` / `resume <id>` `[--fresh]` | execute or continue; `--fresh` disables evidence reuse |
| `grant <id> <permission>` | allow a supervised category for this task only |
| `approve <id> <step> --digest D` | one single-use approval of one step exactly as planned |
| `status`, `report`, `list`, `cancel`, `workflows` | bookkeeping |

Options: `--grant`, `--scratch DIR`, `--var NAME=VALUE`, `--task "wording"`, `--rerun`.
Exit codes: 0 completed, 1 failed/cancelled, 2 bad input, 4 blocked (decision needed), 5 another run holds the lock.

## Task spec

```json
{"title": "...", "task": "original wording", "workflow": "motiva-regression",
 "grants": ["recovery"], "allowed_write_paths": [], "scratch_root": "...", "max_parallel": 4}
```

Instead of `workflow`, give `steps`: `id`, `action`, `params`, `depends_on` (must succeed), `run_after`
(ordering only), `timeout`, `retries` (0-3), `resources` (exclusive names), `expect`, `inputs` + `cacheable`,
`optional`, `when_exists`, `env` (`set`, `prepend_path`). Variables: `${root}`, `${scratch}`, `${vars.x}`.

Actions: `git_snapshot`, `git_compare`, `source_snapshot`, `process_check`, `file_check`, `tool_check`,
`cmake_cache_inspect`, `registry_snapshot` (read-only HKCU, hashes only), `cmake_configure`, `cmake_build`,
`qt_test`, `command` (argv list, no shell strings), `python_script`, `lias_automation`, `edit_file`.
PowerShell is run as a `command` and therefore needs approval.

## Permissions (`policy.py`)

| level | examples | needs |
|---|---|---|
| automatic | read-only actions, read-only git, build/test inside the scratch root, risk-0 automation | nothing |
| supervised | `edit`, `recovery`, `visible-desktop`, `script:<path>`, `automation:<id>` (risk 1) | a grant on the task |
| approval | any other command, git writes, shells, deploy/commit/PR/installer/`build-compile` automations, builds outside scratch | `approve` per step |
| refused | edits of `.git`, secrets, Claude settings, the agent itself, paths not in `allowed_write_paths` | never |

Grants and approvals belong to one task and are never inherited. An approved step is never auto-retried;
if it is interrupted or fails, a new approval is required.

## Verification, recovery, reuse

- A step is `verified` only if checks beyond "it ran" pass: QtTest totals (no failures, at least one pass),
  build artifacts present, file hash after an edit, or explicit `expect`. Otherwise `unverified`.
  `expect.exit_code_is_sufficient: true` is the owner's explicit opt-out.
- Recoveries (`diagnose.py`, need `recovery`): bounded retry for timeout, locked file, toolchain spawn failure,
  busy port; one-time environment fix for missing program/DLL (`params.recovery_path`) or Qt platform plugin
  (`params.qt_plugins`). Compile, link, CMake, test and unknown failures are escalated, never retried.
- Failure signatures are remembered (`agent-data/diagnoses.json`) and flagged "seen before".
- Reuse (cacheable steps only): key = step definition + content hash of every declared input + test exe hash +
  Python version. Needs the original log unchanged and evidence under 7 days old. Builds always run
  (the build tool decides what is stale). Observations and baseline comparisons always run on a new task.
- Duplicate submission: same spec on the same source revision returns the existing task.
- Resume: verified steps are kept unless sources changed between runs; final comparisons always re-run.

## Storage

`.claude/lias/agent-data/` (gitignored): `tasks/<id>/{task.json,report.md,logs/}`, `evidence/`, `*.lock`.
Nothing is deleted automatically. Scratch default: `<system temp>/lias-agent-<project id>`.

## Reuse in another project

Copy `.claude/lias/`, write `agent/workflows/<name>.json` (see `motiva-regression.json`) or submit `steps`.
Engine code has no project-specific logic. `qt_test` is the only test-runner parser; other runners use
`command`/`python_script` with `expect`.

## Reasoning interface

`reasoner.Reasoner.advise(step, diagnosis) -> str`. The default only escalates. Output is report text and is
never executed.

## Limits (do not overstate)

- A guardrail, not a sandbox: builds, tests and scripts execute project code, and the caller who can run the
  CLI can also run `grant`/`approve`.
- Reuse does not detect changes in system DLLs, Qt plugins, drivers or desktop state; use `--fresh`.
- A timeout kills the started process tree (`taskkill /T`); already-orphaned grandchildren may survive.
- Does not create files or generate automation modules (use `automation propose|promote` by hand).
- `lias_automation` steps need the LIAS database; a `PROJECT_MISMATCH` is reported, never auto-rebound.
- Logs are redacted by pattern; an unusual secret format could pass through.

## Tests

`python .claude/lias/tests/test_agent.py` and `test_agent_safety.py` (temporary projects, mocked actions).
