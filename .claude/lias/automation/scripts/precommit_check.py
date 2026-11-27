"""Pre-commit gate: memory line limits, leftover debug markers in changed files, then a build."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _build_guard import safe_build  # noqa: E402
from _common import qt_env, run, tool  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
git = tool("git")
res = {"ok": True}
mem = os.path.join(root, ".claude", "memory")
over = {}
for n in os.listdir(mem) if os.path.isdir(mem) else []:
    if n.endswith(".md"):
        with open(os.path.join(mem, n), encoding="utf-8") as f:
            c = sum(1 for _ in f)
        if c > 200:
            over[n] = c
res["memory_over_limit"] = over
_, names = run([git, "status", "--porcelain"], root)
changed = [x[3:].strip().strip('"') for x in names.splitlines()]
pat = re.compile(r"qDebug\(\)|\bTODO\b|\bFIXME\b|<<<<<<<|>>>>>>>|console\.log")
hits = []
for rel in changed:
    if rel.endswith((".cpp", ".h", ".py", ".js")) and ".claude" not in rel:
        p = os.path.join(root, rel)
        if os.path.isfile(p):
            with open(p, encoding="utf-8", errors="replace") as f:
                for i, ln in enumerate(f, 1):
                    if pat.search(ln):
                        hits.append("%s:%d: %s" % (rel, i, ln.strip()[:120]))
res["debug_markers"] = hits[:50]
if a.get("build", True):
    b = safe_build(root, a.get("build_dir"), qt_env())
    res["build_dir"] = b["build_dir"]
    if "refused" in b:
        res["build_refused"] = b["refused"]
        if "running" in b:
            res["running"] = b["running"]
    else:
        res["build_exit"] = b["exit_code"]
        if b["exit_code"]:
            res["build_errors"] = [x.strip()[:300] for x in b["output"].splitlines() if "error" in x.lower()][:40]
res["ok"] = not over and not res.get("build_exit", 0) and "build_refused" not in res
res["note"] = "debug markers are informational" if hits else ""
print(json.dumps(res))
