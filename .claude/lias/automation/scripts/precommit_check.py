"""Pre-commit gate: memory line limits, leftover debug markers in changed files, then a build."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run, tool  # noqa: E402

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
    env = dict(os.environ)
    env["PATH"] = r"D:\Apps\Qt6\Tools\mingw1310_64\bin;D:\Apps\Qt6\6.12.0\mingw_64\bin;" + env.get("PATH", "")
    code, text = run(["cmake", "--build", "build"], root, 540, env)
    res["build_exit"] = code
    if code:
        res["build_errors"] = [x.strip()[:300] for x in text.splitlines() if "error" in x.lower()][:40]
res["ok"] = not over and not res.get("build_exit", 0)
res["note"] = "debug markers are informational" if hits else ""
print(json.dumps(res))
