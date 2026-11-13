"""Stage everything, commit with the given message (+attribution), optionally push. One JSON line out."""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import SECRET_HINTS, TRAILER, run, tool  # noqa: E402

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
msg = a.get("message", "").strip()
git = tool("git")
fail = {"ok": False}
if not git or not msg:
    print(json.dumps({**fail, "error": "git missing or empty message"}))
    sys.exit(0)
paths = [x.strip() for x in a.get("paths", "").split(",") if x.strip()]
run([git, "add", "--"] + paths if paths else [git, "add", "-A"], root)
_, names = run([git, "diff", "--cached", "--name-only"], root)
files = [n for n in names.splitlines() if n.strip()]
bad = [f for f in files if any(h in f.lower() for h in SECRET_HINTS)]
if not files:
    print(json.dumps({**fail, "error": "nothing staged"}))
elif bad:
    run([git, "reset", "-q"], root)
    print(json.dumps({**fail, "error": "possible secret files staged; unstaged everything", "files": bad}))
else:
    code, text = run([git, "commit", "-q", "-m", msg + "\n\n" + TRAILER], root)
    if code:
        print(json.dumps({**fail, "error": "commit failed", "detail": text[-800:]}))
        sys.exit(0)
    _, h = run([git, "rev-parse", "--short", "HEAD"], root)
    _, br = run([git, "rev-parse", "--abbrev-ref", "HEAD"], root)
    br = br.strip()
    res = {"ok": True, "commit": h.strip(), "branch": br, "files": len(files), "pushed": False}
    if a.get("push", True):
        pc, pt = run([git, "push", "-u", "origin", br], root, timeout=180)  # never forced
        res["pushed"] = pc == 0
        if pc:
            res["ok"] = False
            res["error"] = "push failed"
            res["detail"] = pt[-800:]
    print(json.dumps(res))
