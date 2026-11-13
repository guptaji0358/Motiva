"""Report CI check results for a PR (default: current branch's PR); lists only failures in detail."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import run, tool  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
gh = tool("gh", [r"C:\Program Files\GitHub CLI"])
if not gh:
    print(json.dumps({"ok": False, "error": "gh not found"}))
    sys.exit(0)
cmd = [gh, "pr", "checks"] + ([str(a["pr"])] if a.get("pr") else []) + ["--json", "name,state,link"]
code, text = run(cmd, root, 60)
try:
    checks = json.loads(text)
except ValueError:
    print(json.dumps({"ok": False, "error": "gh failed (no PR or not authenticated?)", "detail": text[-400:]}))
    sys.exit(0)
failed = [c for c in checks if c.get("state") in ("FAILURE", "ERROR", "TIMED_OUT", "CANCELLED")]
pending = [c["name"] for c in checks if c.get("state") in ("PENDING", "QUEUED", "IN_PROGRESS")]
print(json.dumps({"ok": not failed and not pending, "total": len(checks), "failed": failed, "pending": pending}))
