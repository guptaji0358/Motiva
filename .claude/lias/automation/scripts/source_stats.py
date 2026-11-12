"""Count source files and lines by extension. stdin: JSON args. stdout: one JSON line."""
import json
import os
import sys

SKIP = {".git", "build", "deployment", "deploy", "release", "third_party", "node_modules", "__pycache__", ".claude"}
DEFAULT_EXT = [".cpp", ".h", ".hpp", ".c", ".qml", ".py", ".js", ".ps1", ".bat", ".cmake", ".txt", ".html", ".css"]

args = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
exts = tuple(e.strip().lower() for e in args.get("extensions", "").split(",") if e.strip()) or tuple(DEFAULT_EXT)
by_ext = {}
for d, dirs, files in os.walk(root):
    dirs[:] = [x for x in dirs if x not in SKIP]
    for f in files:
        ext = os.path.splitext(f)[1].lower()
        if ext in exts:
            try:
                with open(os.path.join(d, f), "rb") as fh:
                    n = sum(1 for _ in fh)
            except OSError:
                continue
            e = by_ext.setdefault(ext, {"files": 0, "lines": 0})
            e["files"] += 1
            e["lines"] += n
print(json.dumps({"files_scanned": sum(v["files"] for v in by_ext.values()),
                  "total_lines": sum(v["lines"] for v in by_ext.values()),
                  "by_extension": dict(sorted(by_ext.items(), key=lambda kv: -kv[1]["lines"]))}))
