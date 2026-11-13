"""Push the current (non-main) branch and open a PR against main with gh. One JSON line out."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import PR_FOOTER, run, tool  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
git = tool("git")
gh = tool("gh", [r"C:\Program Files\GitHub CLI"])
base = a.get("base", "main")
if not git or not gh:
    print(json.dumps({"ok": False, "error": "git or gh not found"}))
    sys.exit(0)
_, br = run([git, "rev-parse", "--abbrev-ref", "HEAD"], root)
br = br.strip()
if br in (base, "HEAD"):
    print(json.dumps({"ok": False, "error": "on '%s'; create a feature branch first" % br}))
    sys.exit(0)
pc, pt = run([git, "push", "-u", "origin", br], root, timeout=180)
if pc:
    print(json.dumps({"ok": False, "error": "push failed", "detail": pt[-800:]}))
    sys.exit(0)
body = a.get("body", "").strip() + "\n\n" + PR_FOOTER
code, text = run([gh, "pr", "create", "--base", base, "--head", br, "--title", a["title"], "--body", body], root, 120)
url = next((x for x in text.splitlines() if x.startswith("http")), "")
print(json.dumps({"ok": code == 0 and bool(url), "branch": br, "url": url, "detail": "" if url else text[-800:]}))
