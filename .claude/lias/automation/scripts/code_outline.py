"""Compact outline of a source file: definitions, listeners, observers, DOM/storage hooks, with line numbers.
Replaces reading a whole file when only structure is needed. Read-only."""
import json
import os
import re
import sys

args = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
rel = args.get("file", "extension/content.js")
path = os.path.normpath(os.path.join(root, rel))
if not path.startswith(os.path.normpath(root)) or not os.path.isfile(path):
    print(json.dumps({"ok": False, "error": "file not found inside project: " + rel}))
    sys.exit(0)
extra = args.get("pattern") or ""
default = (r"^\s*(async\s+)?function\s+\w+|^\s*(const|let|var)\s+\w+\s*=\s*(async\s*)?(\([^)]*\)|\w+)\s*=>"
           r"|addEventListener|new MutationObserver|\.observe\(|setTimeout|setInterval"
           r"|chrome\.(storage|runtime|contextMenus)\.\w+|style\.display|^\s*(class|def)\s+\w+")
rx = re.compile(extra or default)
limit = int(args.get("max", 120))
hits = []
with open(path, encoding="utf-8", errors="replace") as fh:
    total = 0
    for n, line in enumerate(fh, 1):
        total = n
        if rx.search(line):
            hits.append(f"{n}: {line.strip()[:110]}")
print(json.dumps({"ok": True, "file": rel, "lines": total, "matches": len(hits),
                  "truncated": len(hits) > limit, "outline": hits[:limit]}))
