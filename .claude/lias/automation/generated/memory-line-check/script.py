"""Report memory markdown files over the line limit (project rule: <= 200 lines each)."""
import json
import os
import sys

args = json.loads(sys.stdin.read() or "{}")
limit = args.get("limit", 200)
root = os.environ["LIAS_PROJECT_ROOT"]
mem = os.path.join(root, ".claude", "memory")
files = {}
if os.path.isdir(mem):
    for name in sorted(os.listdir(mem)):
        if name.endswith(".md"):
            with open(os.path.join(mem, name), "rb") as fh:
                files[name] = sum(1 for _ in fh)
over = {k: v for k, v in files.items() if v > limit}
print(json.dumps({"ok": not over, "limit": limit, "files_checked": len(files), "over_limit": over,
                  "max_lines": max(files.values()) if files else 0}))
