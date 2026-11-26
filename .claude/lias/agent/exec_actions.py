"""Actions that execute things or change files. Permission is decided before any of these is called."""
import json
import os
import re
import subprocess
import sys
from pathlib import Path

from ..core import db, runner
from ..core.registry import Registry
from . import inspect_actions as ins
from .inspect_actions import outcome
from .process import run_process
from .util import file_hash, redact

TOTALS_RE = re.compile(r"Totals: (\d+) passed, (\d+) failed, (\d+) skipped, (\d+) blacklisted")
FAIL_RE = re.compile(r"^(?:FAIL!|XPASS)\s*: (.+)$", re.M)
SKIP_RE = re.compile(r"^SKIP\s*: (.+)$", re.M)
LOC_RE = re.compile(r"^\s*Loc: \[(.+)\]$", re.M)


def _run(step, ctx, argv, cwd=None, suffix="run"):
    r = run_process(argv, cwd or ctx["root"], step["timeout"], ctx["env"], ctx["log_base"] + f".{suffix}.log")
    cmd = {"cmd": redact(subprocess.list2cmdline(argv)), "exit_code": r["exit_code"], "log": r["log"]}
    return outcome(r["exit_code"], {"timed_out": r["timed_out"], "duration_s": r["duration_s"]}, r["error"],
                   r["log"], [cmd], r["output"])


def cmake_configure(step, ctx):
    p = step["params"]
    argv = [p.get("cmake", "cmake"), "-S", p["source_dir"], "-B", p["build_dir"], "-G", p.get("generator", "Ninja")]
    return _run(step, ctx, argv + list(p.get("args", [])), suffix="configure")


def cmake_build(step, ctx):
    p = step["params"]
    argv = [p.get("cmake", "cmake"), "--build", p["build_dir"]]
    if p.get("targets"):
        argv += ["--target"] + list(p["targets"])
    if p.get("jobs"):
        argv += ["--parallel", str(int(p["jobs"]))]
    res = _run(step, ctx, argv, suffix="build")
    res["data"]["warnings"] = res["output"].count("warning:")
    res["data"]["up_to_date"] = "ninja: no work to do" in res["output"]
    return res


def qt_test(step, ctx):
    p = step["params"]
    exe = p["exe"]
    results = Path(ctx["log_base"] + ".results.txt")
    results.unlink(missing_ok=True)  # never read a previous attempt's results
    os.makedirs(os.path.dirname(exe) or ".", exist_ok=True)
    res = _run(step, ctx, [exe, "-o", f"{results},txt"], cwd=os.path.dirname(exe), suffix="test")
    text = ""
    if results.is_file():
        text = redact(results.read_text(encoding="utf-8", errors="replace"))
        results.write_text(text, encoding="utf-8")
        res["log"] = str(results)
        res["commands"][0]["log"] = str(results)
    m = TOTALS_RE.search(text)
    totals = dict(zip(("passed", "failed", "skipped", "blacklisted"), map(int, m.groups()))) if m else None
    locs = LOC_RE.findall(text)
    res["data"].update({"totals": totals, "failures": [x.strip()[:300] for x in FAIL_RE.findall(text)][:40],
                        "failure_locations": locs[:40], "skips": [x.strip()[:200] for x in SKIP_RE.findall(text)][:20],
                        "exe_sha256": file_hash(exe)})
    res["output"] = text + "\n" + res["output"]
    return res


def command(step, ctx):
    p = step["params"]
    cwd = os.path.join(ctx["root"], p.get("cwd", "."))
    return _run(step, ctx, list(p["argv"]), cwd=cwd)


def python_script(step, ctx):
    p = step["params"]
    argv = [sys.executable, "-B", os.path.join(ctx["root"], p["script"])] + [str(a) for a in p.get("args", [])]
    return _run(step, ctx, argv)


def lias_automation(step, ctx):
    p = step["params"]
    paths = ctx["paths"]
    try:
        conn = db.connect(paths)  # sqlite connections are per thread
    except db.ProjectMismatch as e:  # never rebind on our own: that rewrites existing LIAS data
        return outcome(1, error=f"LIAS database needs an owner decision: {e}")
    try:
        reg = Registry(paths, conn)
        manifest = reg.load()[0].get(p["id"])
        if manifest is None:
            return outcome(1, error=f"unknown automation {p['id']}")
        env = runner.run(paths, conn, reg, p["id"], p.get("args", {}), allow_risk=manifest["risk"])
    finally:
        conn.close()
    log = ctx["log_base"] + ".automation.json"
    text = redact(json.dumps(env, indent=1))
    Path(log).write_text(text, encoding="utf-8")
    ok = env["status"] == "success" and (env["result"] or {}).get("ok", True) is not False
    err = None if ok else json.dumps(env["error"] or env["result"])[:500]
    return outcome(0 if ok else 1, {"envelope": json.loads(text)}, err, log,
                   [{"cmd": f"lias automation run {p['id']}", "exit_code": 0 if ok else 1, "log": log}], text)


def edit_file(step, ctx):
    p = step["params"]
    rel, old, new = p["path"], p.get("old"), p.get("new")
    if not old or new is None or old == new:
        return outcome(1, error="edit_file needs distinct 'old' (non-empty) and 'new' strings")
    target = Path(ctx["root"]) / rel
    if not target.is_file():
        return outcome(1, error=f"file not found (the agent does not create files): {rel}")
    raw = target.read_bytes()
    old_b, new_b = old.encode(), new.encode()
    if b"\r\n" in raw and "\r" not in old + new:
        old_b, new_b = old_b.replace(b"\n", b"\r\n"), new_b.replace(b"\n", b"\r\n")
    count = raw.count(old_b)
    if new_b and new_b in raw and (count == 0 or old_b in new_b):
        return outcome(0, {"path": rel, "changed": False, "note": "already applied", "sha256": file_hash(target)})
    if count != 1:
        return outcome(1, error=f"'old' must occur exactly once in {rel} (found {count})")
    want = p.get("expect_sha256")
    if want and file_hash(target) != want:
        return outcome(1, error=f"{rel} changed since the task was written (sha256 mismatch)")
    backup = Path(ctx["log_base"] + ".before.bak")
    backup.write_bytes(raw)
    tmp = target.with_name(target.name + ".lias-edit.tmp")
    tmp.write_bytes(raw.replace(old_b, new_b, 1))
    os.replace(tmp, target)
    return outcome(0, {"path": rel, "changed": True, "sha256_before": file_hash(backup),
                       "sha256": file_hash(target), "backup": str(backup)})


REGISTRY = {"git_snapshot": ins.git_snapshot, "git_compare": ins.git_compare,
            "source_snapshot": ins.source_snapshot, "process_check": ins.process_check,
            "file_check": ins.file_check, "tool_check": ins.tool_check,
            "cmake_cache_inspect": ins.cmake_cache_inspect, "registry_snapshot": ins.registry_snapshot,
            "cmake_configure": cmake_configure, "cmake_build": cmake_build, "qt_test": qt_test,
            "command": command, "python_script": python_script, "lias_automation": lias_automation,
            "edit_file": edit_file}
