"""Build, then copy build/Motiva.exe to deployment/resources/bin (what build-and-deploy.bat does)."""
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run  # noqa: E402

json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
env = dict(os.environ)
env["PATH"] = r"D:\Apps\Qt6\Tools\mingw1310_64\bin;D:\Apps\Qt6\6.12.0\mingw_64\bin;" + env.get("PATH", "")
run(["taskkill", "/F", "/IM", "Motiva.exe"], root, 20)
code, text = run(["cmake", "--build", "build"], root, 540, env)
if code:
    errs = [x.strip()[:300] for x in text.splitlines() if "error" in x.lower() or "FAILED" in x][:40]
    print(json.dumps({"ok": False, "exit_code": code, "errors": errs}))
    sys.exit(0)
src = os.path.join(root, "build", "Motiva.exe")
dst = os.path.join(root, "deployment", "resources", "bin", "Motiva.exe")
try:
    shutil.copyfile(src, dst)
except OSError as e:
    print(json.dumps({"ok": False, "error": "copy failed: %s" % e}))
    sys.exit(0)
print(json.dumps({"ok": True, "deployed": dst, "bytes": os.path.getsize(dst)}))
