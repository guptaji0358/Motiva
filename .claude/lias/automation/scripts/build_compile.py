"""Build Motiva with cmake; report exit code and ALL errors if it fails. One JSON line out."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run  # noqa: E402

json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
env = dict(os.environ)
env["PATH"] = r"D:\Apps\Qt6\Tools\mingw1310_64\bin;D:\Apps\Qt6\6.12.0\mingw_64\bin;" + env.get("PATH", "")
run(["taskkill", "/F", "/IM", "Motiva.exe"], root, 20)  # a running exe locks the link step
code, text = run(["cmake", "--build", "build"], root, 540, env)
lines = text.splitlines()
pat = re.compile(r"\berror\b|undefined reference|ld returned|FAILED:|fatal", re.I)
errors = []
for ln in lines:
    s = ln.strip()[:300]
    if pat.search(ln) and s not in errors:
        errors.append(s)
res = {"ok": code == 0, "exit_code": code, "warnings": sum(1 for ln in lines if "warning:" in ln)}
if code:
    res["errors"] = errors[:80]
    res["tail"] = lines[-15:]
else:
    exe = os.path.join(root, "build", "Motiva.exe")
    res["exe"] = exe if os.path.isfile(exe) else ""
print(json.dumps(res))
