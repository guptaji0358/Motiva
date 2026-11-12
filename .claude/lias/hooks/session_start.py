"""SessionStart hook: print a 3-line LIAS summary into Claude's context. Fast, read-mostly, never fails loudly."""
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))


def main():
    if os.environ.get("LIAS_HOOK_ACTIVE"):  # recursion guard
        return
    os.environ["LIAS_HOOK_ACTIVE"] = "1"
    from lias.core import db
    from lias.core.knowledge import Knowledge
    from lias.core.registry import Registry
    paths = db.default_paths()
    if not paths.db_file.exists():
        return
    conn = db.connect(paths)
    Knowledge(conn, paths.project_id).expire_due()
    counts = {r[0]: r[1] for r in conn.execute("SELECT status,COUNT(*) FROM knowledge GROUP BY status")}
    items, bad = Registry(paths, conn).summary()
    ids = ", ".join(f"{i['id']}({i['status']})" for i in items) or "none"
    print("LIAS ready. Knowledge: " + ", ".join(f"{v} {k}" for k, v in sorted(counts.items())) + ".")
    print(f"Automations: {ids}." + (f" Invalid manifests: {len(bad)}." if bad else ""))
    print('Try `python .claude/lias/cli.py knowledge search "<question>"` before broad file reads.')


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass  # a hook must never block the session
