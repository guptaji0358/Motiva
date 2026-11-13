"""Run installer/build-installer.ps1 and report the resulting MotivaSetup.exe. One JSON line out."""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run  # noqa: E402

json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
env = dict(os.environ)
env["PATH"] = r"D:\Apps\Qt6\Tools\mingw1310_64\bin;D:\Apps\Qt6\6.12.0\mingw_64\bin;" + env.get("PATH", "")
start = time.time()
code, text = run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                  os.path.join(root, "installer", "build-installer.ps1")], root, 580, env)
exe = os.path.join(root, "release", "MotivaSetup.exe")
fresh = os.path.isfile(exe) and os.path.getmtime(exe) >= start
res = {"ok": code == 0 and fresh, "exit_code": code}
if res["ok"]:
    res["installer"] = exe
    res["mb"] = round(os.path.getsize(exe) / 1048576, 1)
else:
    res["errors"] = [x.strip()[:300] for x in text.splitlines() if "error" in x.lower() or "fail" in x.lower()][:40]
    res["tail"] = text.splitlines()[-10:]
print(json.dumps(res))
