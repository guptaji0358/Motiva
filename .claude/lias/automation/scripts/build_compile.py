"""Build Motiva with cmake; report exit code and ALL errors if it fails. One JSON line out.

Input (optional): {"build_dir": "<configured build dir, absolute or root-relative>"}; default out/build.
Never kills processes: refuses if a process runs from the build directory.
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _build_guard import safe_build  # noqa: E402
from _common import qt_env  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
res = safe_build(root, a.get("build_dir"), qt_env())
if "refused" in res:
    print(json.dumps(dict(res, ok=False)))
    sys.exit(0)
code, lines = res["exit_code"], res.pop("output").splitlines()
pat = re.compile(r"\berror\b|undefined reference|ld returned|FAILED:|fatal", re.I)
errors = []
for ln in lines:
    s = ln.strip()[:300]
    if pat.search(ln) and s not in errors:
        errors.append(s)
res.update(ok=code == 0, warnings=sum(1 for ln in lines if "warning:" in ln))
if code:
    res["errors"] = errors[:80]
    res["tail"] = lines[-15:]
else:
    exe = os.path.join(res["build_dir"], "Motiva.exe")
    res["exe"] = exe if os.path.isfile(exe) else ""
print(json.dumps(res))
