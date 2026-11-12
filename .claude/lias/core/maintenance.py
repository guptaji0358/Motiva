"""Backup, restore and benchmark helpers."""
import json
import shutil
import sqlite3
import statistics
import time
from pathlib import Path

KEEP_BACKUPS = 10


def backup_create(paths, conn, tag="manual"):
    paths.backups.mkdir(parents=True, exist_ok=True)
    dest = paths.backups / f"lias-{tag}-{time.strftime('%Y%m%d-%H%M%S')}.sqlite3"
    target = sqlite3.connect(dest)
    try:
        conn.backup(target)  # consistent snapshot, safe while the DB is open
    finally:
        target.close()
    old = sorted(paths.backups.glob(f"lias-{tag}-*.sqlite3"))[:-KEEP_BACKUPS]
    for f in old:
        f.unlink()
    return dest.name


def backup_list(paths):
    return sorted(p.name for p in paths.backups.glob("lias-*.sqlite3")) if paths.backups.is_dir() else []


def backup_restore(paths, name):
    """Replace the live DB with a verified backup (current DB is saved first). Call with NO open connection."""
    src = paths.backups / Path(name).name  # bare name only: no path traversal
    if not src.is_file():
        return {"restored": False, "reason": "no such backup"}
    try:
        c = sqlite3.connect(src)
        ok = c.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        pid = c.execute("SELECT value FROM meta WHERE key='project_id'").fetchone()
        c.close()
    except sqlite3.Error as e:
        return {"restored": False, "reason": f"backup unreadable: {e}"}
    if not ok:
        return {"restored": False, "reason": "backup failed integrity check"}
    if pid is None or pid[0] != paths.project_id:
        return {"restored": False, "reason": "backup belongs to a different project"}
    saved = None
    if paths.db_file.exists():
        saved = f"lias-pre-restore-{time.strftime('%Y%m%d-%H%M%S')}.sqlite3"
        shutil.copy2(paths.db_file, paths.backups / saved)
    tmp = paths.db_file.with_suffix(".restore.tmp")
    shutil.copy2(src, tmp)
    tmp.replace(paths.db_file)
    return {"restored": True, "from": src.name, "previous_saved_as": saved}


def _median_ms(fn, n):
    times = []
    for _ in range(n):
        t = time.perf_counter()
        fn()
        times.append((time.perf_counter() - t) * 1000)
    return round(statistics.median(times), 2)


def benchmark(paths, conn, kn, reg, run_automation, runs=3):
    """Measured local metrics only. Model tokens are NOT measured; context size is a character proxy."""
    rows = conn.execute("SELECT id,title,content FROM knowledge WHERE status IN ('documented','verified') "
                        "LIMIT 40").fetchall()
    hits = 0
    for r in rows:
        if r["id"] in [h["id"] for h in kn.search(r["title"], limit=3)]:
            hits += 1
    queries = [r["title"] for r in rows] or ["build"]
    search_ms = _median_ms(lambda: [kn.search(q) for q in queries[:10]], runs)
    memory_chars = sum(f.stat().st_size for f in paths.memory.glob("*.md")) if paths.memory.is_dir() else 0
    sample = kn.search(queries[0]) if queries else []
    result_chars = sum(len(h["content"]) for h in sample)
    cold = _median_ms(lambda: run_automation("source-stats", False), runs)
    run_automation("source-stats", True)
    warm = _median_ms(lambda: run_automation("source-stats", True), runs)
    report = {
        "ts": time.time(), "runs_per_metric": runs,
        "retrieval": {"records_tested": len(rows), "top3_accuracy": round(hits / len(rows), 3) if rows else None,
                      "ten_searches_median_ms": search_ms},
        "context_proxy_chars": {"all_memory_files": memory_chars, "one_search_result": result_chars},
        "source_stats_ms": {"cold": cold, "cached": warm, "speedup_x": round(cold / warm, 1) if warm else None},
        "not_measured": ["model input/output tokens", "tool-call counts", "setup cost vs recurring savings in tokens"],
    }
    paths.benchmarks.mkdir(parents=True, exist_ok=True)
    (paths.benchmarks / f"bench-{time.strftime('%Y%m%d-%H%M%S')}.json").write_text(json.dumps(report, indent=1))
    return report
