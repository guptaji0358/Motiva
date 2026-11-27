"""Run scripts/installer/build-installer.ps1 and report the resulting MotivaSetup.exe. One JSON line out.

Input (optional): {"build_dir": "...", "output_dir": "..."} (absolute or root-relative); defaults out/build and
out/release. The script never kills processes and refuses if one runs from a directory it would overwrite.
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import qt_env, run  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
env = qt_env()
cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
       os.path.join(root, "scripts", "installer", "build-installer.ps1")]
for key, flag in (("build_dir", "-BuildDir"), ("output_dir", "-OutputDir")):
    if a.get(key):
        cmd += [flag, a[key]]
start = time.time()
code, text = run(cmd, root, 580, env)
out_dir = os.path.normpath(os.path.join(root, a.get("output_dir") or os.path.join("out", "release")))
exe = os.path.join(out_dir, "MotivaSetup.exe")
fresh = os.path.isfile(exe) and os.path.getmtime(exe) >= start - 1
res = {"ok": code == 0 and fresh, "exit_code": code}
if res["ok"]:
    res["installer"] = exe
    res["mb"] = round(os.path.getsize(exe) / 1048576, 1)
else:
    refused = [x.strip() for x in text.splitlines() if x.startswith("REFUSED:")]
    if refused:
        res["refused"] = refused[0][:1500]
    res["errors"] = [x.strip()[:300] for x in text.splitlines() if "error" in x.lower() or "fail" in x.lower()][:40]
    res["tail"] = text.splitlines()[-10:]
print(json.dumps(res))
