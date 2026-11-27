---
name: motiva-git-review
description: Read-only final Git review of Motiva before a commit or PR, and detection of stale agent/memory notes. Use when the user asks to review accumulated changes, group files into commits, check for secrets or unwanted files, find what is safe to commit, or refresh outdated notes (suite counts, HEAD references). Runs the LIAS automation git-review first, then applies judgement only where the script cannot.
---

# Motiva final Git review

## 1. Run the automation first

```bash
python .claude/lias/cli.py automation run git-review --args '{"report":true,"remote":true}'
```

Optional `"base":"<ref>"` (default: merge-base with `origin/main`). Humans can run `git-review.bat`.
`remote` uses the network; leave it out when offline. Success needs `status: success` and `result.ok` true.
Long lists are capped in the JSON; `result.report_path` (system temp dir) holds everything.

## 2. Check what the script cannot

- **Stale refs:** local `origin/*` are not fetched. Compare `remote_heads` with them, and use
  `gh pr list --state all` to see whether the branch is already merged. A clean tree usually means the
  work is committed: review `base..HEAD` and say so.
- **Diff content:** read the diffs of the grouped files. Look for behaviour changes hidden inside
  cleanup or docs commits.
- **Secret hits** are `file:line` only. Judge each without printing the value. No hit is not proof;
  history is not scanned.
- **Deleted references** may be comments or other harmless text: open each before calling it broken.
- **Verification status:** take it from `.claude/memory/` and LIAS agent reports, and attribute it. The
  review itself builds and tests nothing.
- **Local-only branches and ignored files** that matter (`git log HEAD..<branch>`; machine-only configs
  under `out/`).

## 3. Report

Branch and latest commit; file counts; changes grouped by purpose; secrets and unwanted files; missing or
incomplete items; commit grouping with exact paths; files safe to commit (`candidates_to_commit`, minus
anything you reject).

## Rules

- A review is read-only: no edit, restore, delete, stage, commit, push, merge, PR, fetch or build.
  Wait for approval.
- After approval to fix stale notes: new branch, change only notes that are wrong today (dated history
  entries that were true when written stay), then re-run the automation. Commit only when asked, through
  the `git-commit-push` automation.
