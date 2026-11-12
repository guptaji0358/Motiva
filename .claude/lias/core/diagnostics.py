"""LIAS diagnostics: doctor (health checks) and status (compact summary)."""
import sqlite3
import sys

from .db import LIAS_VERSION
from .registry import Registry


def status(paths, conn):
    reg = Registry(paths, conn)
    items, bad = reg.summary()
    q = lambda sql: conn.execute(sql).fetchone()[0]  # noqa: E731
    by_status = {r[0]: r[1] for r in conn.execute("SELECT status,COUNT(*) FROM knowledge GROUP BY status")}
    runs = [dict(r) for r in conn.execute(
        "SELECT automation_id,status,duration_ms,cached FROM runs ORDER BY id DESC LIMIT 5")]
    cached = q("SELECT COUNT(*) FROM runs WHERE cached=1")
    total = q("SELECT COUNT(*) FROM runs")
    return {"lias_version": LIAS_VERSION, "project_root": str(paths.root), "project_id": paths.project_id,
            "knowledge": by_status, "automations": items, "bad_manifests": bad,
            "cache_entries": q("SELECT COUNT(*) FROM cache"),
            "cache_hit_rate": round(cached / total, 3) if total else None,
            "recent_runs": runs}


def doctor(paths, conn):
    checks = []

    def chk(name, ok, detail=""):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    chk("python>=3.9", sys.version_info >= (3, 9), sys.version.split()[0])
    try:
        conn.execute("CREATE VIRTUAL TABLE temp.t_fts USING fts5(a)")
        conn.execute("DROP TABLE temp.t_fts")
        chk("sqlite-fts5", True, sqlite3.sqlite_version)
    except sqlite3.Error as e:
        chk("sqlite-fts5", False, str(e))
    integ = conn.execute("PRAGMA integrity_check").fetchone()[0]
    chk("db-integrity", integ == "ok", integ)
    chk("db-schema", conn.execute("PRAGMA user_version").fetchone()[0] >= 1)
    pid = conn.execute("SELECT value FROM meta WHERE key='project_id'").fetchone()
    chk("project-identity", pid and pid[0] == paths.project_id)
    orphan = conn.execute("SELECT COUNT(*) FROM kfts WHERE id NOT IN (SELECT id FROM knowledge)").fetchone()[0]
    missing = conn.execute("SELECT COUNT(*) FROM knowledge WHERE status!='invalidated' AND id NOT IN "
                           "(SELECT id FROM kfts)").fetchone()[0]
    chk("fts-consistent", orphan == 0 and missing == 0, f"orphans={orphan} missing={missing}")
    items, bad = Registry(paths, conn).summary()
    chk("manifests-valid", not bad, str(bad) if bad else f"{len(items)} automations")
    quarantined = [i["id"] for i in items if i["status"] == "quarantined"]
    chk("no-quarantined", not quarantined, ",".join(quarantined))
    hooks = paths.claude / "settings.json"
    chk("hooks-configured", hooks.exists() and "lias" in hooks.read_text(encoding="utf-8", errors="replace"),
        "informational: SessionStart/PostToolUse hooks")
    return {"ok": all(c["ok"] for c in checks if c["check"] != "hooks-configured"), "checks": checks}
