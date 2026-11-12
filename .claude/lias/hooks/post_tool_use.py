"""PostToolUse hook (matcher: Bash): record the command for repeat-workflow discovery. Silent, fast, never fails."""
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))


def main():
    if os.environ.get("LIAS_HOOK_ACTIVE"):
        return
    os.environ["LIAS_HOOK_ACTIVE"] = "1"
    data = json.loads(sys.stdin.read() or "{}")
    cmd = (data.get("tool_input") or {}).get("command")
    if not isinstance(cmd, str):
        return
    from lias.core import db
    from lias.learning import observer
    paths = db.default_paths()
    if paths.db_file.exists():
        observer.record(db.connect(paths), cmd)


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
