"""Generated-automation lifecycle: static validation -> isolated test -> report -> promote/retire.

Candidates live in automation/generated/<id>/{manifest.json,script.py}. They become active only through
`promote`, which requires a passing report whose hashes match the current files. Static checks are a
guardrail against accidents, NOT a sandbox: only risk-0 (read-only) automations can be promoted automatically.
"""
import ast
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from .registry import ManifestError, Registry, validate_manifest

MAX_GENERATED = 20
DUP_JACCARD = 0.7
ALLOWED_IMPORTS = {"json", "os", "sys", "re", "math", "collections", "itertools", "datetime", "pathlib", "glob",
                   "fnmatch", "hashlib", "statistics", "time", "csv", "textwrap", "string", "functools", "typing"}
FORBIDDEN_CALLS = {"eval", "exec", "compile", "__import__", "input", "breakpoint", "globals", "locals"}
FORBIDDEN_ATTRS = {"system", "remove", "unlink", "rmdir", "rename", "popen", "kill", "startfile",
                   "chmod", "write_text", "write_bytes", "mkdir", "makedirs", "touch", "truncate", "removedirs",
                   "execv", "execl", "spawnv", "putenv", "symlink_to", "link_to"}


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def static_check(source):
    """Return a list of problems (empty = passed)."""
    try:
        tree = ast.parse(source)
    except SyntaxError as e:
        return [f"syntax error: {e}"]
    problems = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for a in node.names:
                if a.name.split(".")[0] not in ALLOWED_IMPORTS:
                    problems.append(f"line {node.lineno}: import {a.name} not allowed")
        elif isinstance(node, ast.ImportFrom):
            if node.level or (node.module or "").split(".")[0] not in ALLOWED_IMPORTS:
                problems.append(f"line {node.lineno}: import from {node.module} not allowed")
        elif isinstance(node, ast.Call):
            f = node.func
            if isinstance(f, ast.Name) and f.id in FORBIDDEN_CALLS:
                problems.append(f"line {node.lineno}: call {f.id}() not allowed")
            if isinstance(f, ast.Name) and f.id == "open":
                mode = node.args[1] if len(node.args) > 1 else next((k.value for k in node.keywords if k.arg == "mode"), None)
                if mode is not None and not (isinstance(mode, ast.Constant) and set(str(mode.value)) <= set("rbt")):
                    problems.append(f"line {node.lineno}: open() must be read-only")
        elif isinstance(node, ast.Attribute) and node.attr in FORBIDDEN_ATTRS:
            problems.append(f"line {node.lineno}: attribute .{node.attr} not allowed")
    return problems


def _tokens(text):
    return set(re.findall(r"[a-z]{3,}", text.lower()))


def find_duplicate(manifest, existing):
    mine = _tokens(manifest["description"] + " " + manifest["name"])
    for other in existing.values():
        if other["id"] == manifest["id"]:
            continue
        theirs = _tokens(other["description"] + " " + other["name"])
        if mine and theirs and len(mine & theirs) / len(mine | theirs) >= DUP_JACCARD:
            return other["id"]
    return None


def _isolated_test(cdir, manifest):
    """Run each manifest test in a throwaway project dir built from the test's fixtures."""
    results = []
    for i, t in enumerate(manifest.get("tests", [])):
        tmp = tempfile.mkdtemp(prefix="lias-gen-")
        try:
            for name, content in (t.get("fixtures") or {}).items():
                p = Path(tmp, name)
                if Path(tmp).resolve() not in p.resolve().parents:
                    raise ValueError("fixture path escapes the test directory")
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text(content, encoding="utf-8")
            p = subprocess.run([sys.executable, "-I", str(cdir / "script.py")], input=json.dumps(t.get("args", {})),
                               capture_output=True, text=True, timeout=manifest["timeout"], cwd=tmp,
                               env=dict(os.environ, LIAS_PROJECT_ROOT=tmp))
            lines = [ln for ln in p.stdout.splitlines() if ln.strip()]
            out = json.loads(lines[-1]) if p.returncode == 0 and lines else None
            ok = isinstance(out, dict) and all(out.get(k) == v for k, v in (t.get("expect") or {}).items())
            results.append({"test": i, "ok": ok, "detail": "" if ok else (p.stderr.strip()[-200:] or str(out)[:200])})
        except Exception as e:  # timeout, bad JSON, bad fixture
            results.append({"test": i, "ok": False, "detail": f"{type(e).__name__}: {e}"[:200]})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    return results


def propose(paths, conn, aid):
    """Validate + test a candidate and write report.json. Never activates."""
    cdir = paths.generated / aid
    report = {"id": aid, "ts": time.time(), "passed": False, "stages": {}}
    try:
        manifest = json.loads((cdir / "manifest.json").read_text(encoding="utf-8"))
        script = cdir / "script.py"
        manifest_for_check = dict(manifest, entry="script.py")
        # manifest shape (entry file existence is checked against the candidate dir below)
        validate_manifest(manifest_for_check)
        if manifest["id"] != aid:
            raise ManifestError("manifest id must match directory name")
        if manifest["risk"] != 0:
            raise ManifestError("only risk 0 (read-only) automations can be generated")
        if not manifest.get("tests"):
            raise ManifestError("at least one test is required")
        report["stages"]["manifest"] = "ok"
        problems = static_check(script.read_text(encoding="utf-8"))
        report["stages"]["static"] = problems or "ok"
        reg_items, _ = Registry(paths, conn).load()
        n_gen = sum(1 for m in reg_items.values() if m.get("generated") and m["id"] != aid)
        if n_gen >= MAX_GENERATED:
            problems.append(f"generated-automation budget exhausted ({MAX_GENERATED})")
        dup = find_duplicate(manifest, reg_items)
        report["stages"]["duplicate"] = f"similar to {dup}" if dup else "ok"
        if problems or dup:
            return _write(cdir, report)
        tests = _isolated_test(cdir, manifest)
        report["stages"]["tests"] = tests
        report["passed"] = all(t["ok"] for t in tests)
        report["hashes"] = {"manifest": _sha(cdir / "manifest.json"), "script": _sha(script)}
    except (OSError, json.JSONDecodeError, ManifestError) as e:
        report["stages"]["error"] = str(e)
    return _write(cdir, report)


def _write(cdir, report):
    cdir.mkdir(parents=True, exist_ok=True)
    tmp = cdir / "report.json.tmp"
    tmp.write_text(json.dumps(report, indent=1), encoding="utf-8")
    os.replace(tmp, cdir / "report.json")
    return report


def promote(paths, conn, aid):
    cdir = paths.generated / aid
    try:
        report = json.loads((cdir / "report.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"promoted": False, "reason": "no report; run propose first"}
    if not report.get("passed"):
        return {"promoted": False, "reason": "report did not pass"}
    h = report["hashes"]
    if h["manifest"] != _sha(cdir / "manifest.json") or h["script"] != _sha(cdir / "script.py"):
        return {"promoted": False, "reason": "files changed since validation; run propose again"}
    source = (cdir / "script.py").read_text(encoding="utf-8")
    if static_check(source):  # re-check at activation time
        return {"promoted": False, "reason": "static check failed on re-check"}
    manifest = json.loads((cdir / "manifest.json").read_text(encoding="utf-8"))
    manifest.update(entry=f"{aid.replace('-', '_')}.py", status="active", generated=True)
    dest_script, dest_manifest = paths.scripts / manifest["entry"], paths.manifests / f"{aid}.json"
    if dest_manifest.exists():  # preserve last working version
        paths.backups.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        shutil.copy2(dest_manifest, paths.backups / f"{aid}-{stamp}.json")
        if dest_script.exists():
            shutil.copy2(dest_script, paths.backups / f"{aid}-{stamp}.py")
    for dest, text in ((dest_script, source), (dest_manifest, json.dumps(manifest, indent=2))):
        tmp = dest.with_suffix(dest.suffix + ".tmp")
        tmp.write_text(text, encoding="utf-8")
        os.replace(tmp, dest)
    try:
        validate_manifest(manifest, paths)
    except ManifestError as e:
        dest_manifest.unlink(missing_ok=True)
        dest_script.unlink(missing_ok=True)
        return {"promoted": False, "reason": f"post-copy validation failed: {e}"}
    conn.execute("INSERT INTO audit(ts,action,record_id,detail) VALUES(?,?,?,?)",
                 (time.time(), "promote", aid, f"script sha {h['script'][:12]}"))
    conn.commit()
    return {"promoted": True, "id": aid, "version": manifest["version"]}


def retire(paths, conn, aid):
    """Deactivate a generated automation by moving its files to backups/."""
    mpath = paths.manifests / f"{aid}.json"
    try:
        m = json.loads(mpath.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {"retired": False, "reason": "not found"}
    if not m.get("generated"):
        return {"retired": False, "reason": "not a generated automation; edit it by hand"}
    paths.backups.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    shutil.move(str(mpath), paths.backups / f"{aid}-retired-{stamp}.json")
    s = paths.scripts / m["entry"]
    if s.exists():
        shutil.move(str(s), paths.backups / f"{aid}-retired-{stamp}.py")
    conn.execute("INSERT INTO audit(ts,action,record_id,detail) VALUES(?,?,?,?)", (time.time(), "retire", aid, ""))
    conn.commit()
    return {"retired": True, "id": aid}
