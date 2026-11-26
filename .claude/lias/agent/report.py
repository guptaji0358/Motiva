"""Reporting engine: one concise Markdown report per run, built only from recorded results."""
import os

from .util import atomic_write_text, redact

OK = ("verified", "reused", "not_applicable")


def _cell(text, limit=160):
    return str(text).replace("|", "\\|").replace("\n", " ")[:limit]


def _commands(rec):
    return [c for a in rec["attempts"] for c in a["commands"]]


def build_report(task, plan, log_dir):
    spec, records = task["spec"], task["steps"]
    steps = {s["id"]: s for s in spec["steps"]}
    order = [p["id"] for p in sorted(plan.values(), key=lambda p: p["wave"])]
    count = {}
    for r in records.values():
        count[r["state"]] = count.get(r["state"], 0) + 1
    rel = lambda p: os.path.relpath(p, log_dir).replace("\\", "/") if p and p.startswith(log_dir) else p  # noqa
    L = [f"# LIAS agent report: {task['title']}", "",
         f"**Task** `{task['id']}` | **Result: {task['state'].upper()}** | run {task['runs']} | {task['updated']}",
         "Steps: " + ", ".join(f"{v} {k}" for k, v in sorted(count.items())),
         f"Logs: `{log_dir}` (log names below are relative to it)", "",
         "## 1. Original task", spec["task"],
         f"Grants: {', '.join(spec['grants']) or 'none'} | scratch: `{spec['scratch_root']}`", "",
         "## 2. Actions performed", "| step | state | note |", "|---|---|---|"]
    for sid in order:
        r = records[sid]
        note = r["note"] or ""
        if r["reused_from"]:
            note = f"reused evidence of {r['reused_from']['task']} ({r['reused_from']['when']}); not re-executed"
        elif len(r["attempts"]) > 1:
            note = f"{len(r['attempts'])} attempts"
        L.append(f"| {sid} | {r['state']} | {_cell(note)} |")

    L += ["", "## 3. Files changed"]
    edits = [r["data"] for s, r in records.items()
             if steps[s]["action"] == "edit_file" and r["data"].get("changed")]
    L += [f"- edited by the agent: `{d['path']}` (backup `{rel(d['backup'])}`)" for d in edits] \
        or ["- no files were edited by the agent"]
    for sid in order:
        d = records[sid]["data"]
        if steps[sid]["action"] == "git_compare" and "unchanged" in d:
            if d["unchanged"]:
                L.append(f"- `{sid}`: Git HEAD, branch, index and working tree identical to the baseline")
            else:
                changes = {k: v for k, v in d.items() if v and k != "unchanged"}
                L.append(f"- `{sid}`: Git state DIFFERS from the baseline: {_cell(changes, 600)}")
        elif steps[sid]["action"] == "registry_snapshot" and "unchanged" in d:
            L.append(f"- `{sid}`: registry subtree {'unchanged' if d['unchanged'] else 'CHANGED'} since baseline")

    L += ["", "## 4. Commands executed"]
    n = 0
    for sid in order:
        for c in _commands(records[sid]):
            n += 1
            L.append(f"- `{sid}` exit {c['exit_code']}: `{_cell(c['cmd'], 200)}` -> {rel(c['log'])}")
    if not n:
        L.append("- none in this task (all results blocked or reused)")

    L += ["", "## 5. Tests and build results"]
    tests = [s for s in order if steps[s]["action"] == "qt_test"]
    if tests:
        L += ["| test | state | passed | failed | skipped |", "|---|---|---|---|---|"]
        total = [0, 0, 0]
        for sid in tests:
            r = records[sid]
            t = r["data"].get("totals") if r["state"] in OK + ("failed", "unverified") else None
            if t:
                total = [total[0] + t["passed"], total[1] + t["failed"], total[2] + t["skipped"]]
            vals = [t["passed"], t["failed"], t["skipped"]] if t else ["-"] * 3
            L.append(f"| {sid} | {r['state']} | {vals[0]} | {vals[1]} | {vals[2]} |")
        L.append(f"| **total ({len(tests)} suites)** | | {total[0]} | {total[1]} | {total[2]} |")
        for sid in tests:
            for s in records[sid]["data"].get("skips", []):
                L.append(f"- skipped in `{sid}`: {_cell(s)}")
    for sid in order:
        r, a = records[sid], steps[sid]["action"]
        if a in ("cmake_configure", "cmake_build"):
            extra = ""
            if a == "cmake_build" and r["data"]:
                extra = f", {r['data'].get('warnings', 0)} warning lines" + \
                        (", already up to date" if r["data"].get("up_to_date") else "")
            files = (r["verification"] or {}).get("evidence", {}).get("files", {})
            if files:
                extra += ", artifacts: " + ", ".join(os.path.basename(f) for f in files)
            L.append(f"- `{sid}`: {r['state']}{extra}")
        elif a == "process_check" and r["data"]:
            found = r["data"]["running"]
            L.append(f"- `{sid}`: " + (", ".join(f"{x['name']} (pid {x['pid']})" for x in found) or "none running")
                     + "; nothing was terminated")
        elif a == "cmake_cache_inspect" and r["data"]:
            L += [f"- `{sid}` finding: {_cell(x, 250)}" for x in r["data"].get("findings", [])] \
                or [f"- `{sid}`: no findings"]

    L += ["", "## 6. Failures encountered"]
    failures, fixes = [], []
    for sid in order:
        r = records[sid]
        for d in r["diagnoses"]:
            seen = d.get("seen_before")
            failures.append(f"- `{sid}`: **{d['pattern']}** ({d['kind']}): {_cell(d['summary'], 250)}"
                            + (f" [seen before, first in {seen['first_task']}"
                               + (f"; fixed then by {seen['fixed_by']}" if seen["fixed_by"] else "") + "]"
                               if seen else ""))
            failures += [f"    - {_cell(x, 250)}" for x in d["details"][:8]]
            if len(d["details"]) > 8:
                failures.append(f"    - ... {len(d['details']) - 8} more in the log")
            if d.get("fix"):
                fixes.append(f"- `{sid}`: {d['fix']['applied']['type']} for {d['pattern']} -> {d['fix']['result']}")
    L += failures or ["- none"]
    L += ["", "## 7. Fixes attempted"] + (fixes or ["- none"])

    problems, decisions = [], []
    for sid in order:
        r, optional = records[sid], " (optional step)" if steps[sid]["optional"] else ""
        if r["state"] in OK:
            continue
        why = r["note"] or ""
        if r["verification"] and r["state"] in ("failed", "unverified"):
            bad = [c for c in r["verification"]["checks"] if not c["ok"]]
            why = "; ".join(f"{c['check']}: expected {c['expected']}, got {c['actual']}" for c in bad)
        problems.append(f"- `{sid}` {r['state']}{optional}: {_cell(why, 300)}")
        if r["state"] == "blocked":
            p = plan[sid]
            if p["permission"] == "approval":
                decisions.append(f"- Approve `{sid}`? {p['reason']}. After reviewing the plan: "
                                 f"`agent approve {task['id']} {sid} --digest {p['digest']}`")
            else:
                decisions.append(f"- Grant `{p['grant']}` for `{sid}`? {p['reason']}. "
                                 f"`agent grant {task['id']} {p['grant']}`")
        elif r["state"] == "unverified":
            decisions.append(f"- `{sid}` ran but its outcome cannot be proven; add an expectation or accept manually")
        elif r["state"] == "failed":
            stopped = r["diagnoses"][-1].get("stopped") if r["diagnoses"] else None
            decisions.append(f"- `{sid}`: {r.get('advice') or 'inspect the log'}"
                             + (f" (agent stopped: {stopped})" if stopped else ""))
    for sid in order:
        d = records[sid]["data"]
        if steps[sid]["action"] == "cmake_cache_inspect" and d.get("findings"):
            problems.append(f"- advisory: `{sid}` reports {len(d['findings'])} stale-configuration finding(s)")
        if steps[sid]["action"] == "qt_test" and (d.get("totals") or {}).get("skipped"):
            problems.append(f"- advisory: `{sid}` skipped {d['totals']['skipped']} test(s); those paths are untested")
    L += ["", "## 8. Remaining problems"] + (problems or ["- none detected by this task's checks"])
    L += ["", "## 9. Human decisions required"] + (decisions or ["- none"])
    if task["state"] == "completed":
        nxt = "Review any advisory items above. Nothing was committed, pushed or deployed by this task."
        if any(x["permission"] == "approval" for x in plan.values()):
            nxt = "Review the results; approval-level steps ran only with the recorded approvals."
    elif task["state"] == "blocked":
        nxt = f"Answer section 9, then `agent resume {task['id']}`."
    else:
        nxt = f"Fix the failures in section 6, then `agent resume {task['id']}` (verified work is not repeated)."
    L += ["", "## 10. Recommended next action", nxt, ""]
    return redact("\n".join(L))


def write_report(store, task, plan):
    folder = store.task_dir(task["id"])
    path = folder / "report.md"
    atomic_write_text(path, build_report(task, plan, str(folder / "logs") + os.sep))
    return path
