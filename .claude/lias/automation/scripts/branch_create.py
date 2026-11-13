"""Create and switch to a feature branch from main (uncommitted changes carry over). One JSON line out."""
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run, tool  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
git = tool("git")
name = re.sub(r"[^a-z0-9]+", "-", a.get("name", "").lower()).strip("-")
if not git or not name:
    print(json.dumps({"ok": False, "error": "git missing or empty name"}))
    sys.exit(0)
branch = "%s/%s" % (a.get("prefix", "feature"), name)
code, text = run([git, "checkout", "-b", branch], root)
print(json.dumps({"ok": code == 0, "branch": branch, "detail": "" if code == 0 else text[-400:]}))
