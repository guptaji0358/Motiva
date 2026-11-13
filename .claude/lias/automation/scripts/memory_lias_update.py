"""Optionally append a line to a memory file, enforce the 200-line limit, reindex LIAS knowledge, run doctor."""
import json
import os
import subprocess
import sys

a = json.loads(sys.stdin.read() or "{}")
root = os.environ["LIAS_PROJECT_ROOT"]
mem = os.path.join(root, ".claude", "memory")
cli = os.path.join(root, ".claude", "lias", "cli.py")
res = {"ok": True, "appended": False}
f, text = a.get("file", ""), a.get("append", "")
if f and text:
    p = os.path.abspath(os.path.join(mem, f))
    if os.path.commonpath([p, mem]) != mem or not p.endswith(".md") or not os.path.isfile(p):
        print(json.dumps({"ok": False, "error": "file must be an existing .md inside .claude/memory"}))
        sys.exit(0)
    with open(p, encoding="utf-8") as fh:
        content = fh.read()
    new = content.rstrip("\n") + "\n" + text.strip() + "\n"
    if new.count("\n") > 200:
        print(json.dumps({"ok": False, "error": "append would exceed 200 lines; split the file"}))
        sys.exit(0)
    with open(p, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new)
    res["appended"] = True
over = {}
for n in sorted(os.listdir(mem)):
    if n.endswith(".md"):
        with open(os.path.join(mem, n), encoding="utf-8") as fh:
            c = sum(1 for _ in fh)
        if c > 200:
            over[n] = c
res["over_limit"] = over
if over:
    res["ok"] = False


def cli_run(*args):
    r = subprocess.run([sys.executable, cli, *args], cwd=root, capture_output=True, text=True, timeout=60)
    return r.returncode, r.stdout.strip()[-400:]


res["index_exit"], res["index"] = cli_run("knowledge", "index")
res["doctor_exit"], res["doctor"] = cli_run("doctor")
res["ok"] = res["ok"] and res["index_exit"] == 0 and res["doctor_exit"] == 0
print(json.dumps(res))
