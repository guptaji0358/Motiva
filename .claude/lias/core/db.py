"""LIAS storage: project paths, project identity, SQLite connection, migrations."""
import hashlib
import os
import shutil
import sqlite3
import time
from pathlib import Path

LIAS_VERSION = "2.0.0"

MIGRATIONS = [
    # v1
    """
    CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL);
    CREATE TABLE knowledge(
        id TEXT PRIMARY KEY, category TEXT NOT NULL, title TEXT NOT NULL,
        content TEXT NOT NULL, source TEXT NOT NULL, created REAL NOT NULL,
        verified_at REAL, status TEXT NOT NULL, confidence REAL NOT NULL DEFAULT 0.5,
        expires_at REAL, project_id TEXT NOT NULL, related TEXT NOT NULL DEFAULT '[]',
        version INTEGER NOT NULL DEFAULT 1);
    CREATE VIRTUAL TABLE kfts USING fts5(id UNINDEXED, title, content);
    CREATE TABLE audit(id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL,
        action TEXT NOT NULL, record_id TEXT, detail TEXT);
    CREATE TABLE automation_state(id TEXT PRIMARY KEY, status TEXT, failures INTEGER NOT NULL DEFAULT 0,
        last_run REAL, last_status TEXT);
    CREATE TABLE runs(id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL NOT NULL,
        automation_id TEXT NOT NULL, status TEXT NOT NULL, duration_ms INTEGER, cached INTEGER NOT NULL DEFAULT 0,
        error TEXT);
    CREATE TABLE cache(key TEXT PRIMARY KEY, automation_id TEXT NOT NULL, created REAL NOT NULL,
        expires_at REAL NOT NULL, result TEXT NOT NULL);
    """,
    # v2: workflow observation
    """
    CREATE TABLE observed(cmd TEXT PRIMARY KEY, count INTEGER NOT NULL DEFAULT 0,
        first_seen REAL NOT NULL, last_seen REAL NOT NULL, dismissed INTEGER NOT NULL DEFAULT 0);
    """,
]


class ProjectMismatch(RuntimeError):
    pass


class Paths:
    """All LIAS runtime locations for one project. Everything lives under <root>/.claude/lias."""

    def __init__(self, root):
        self.root = Path(root).resolve()
        self.claude = self.root / ".claude"
        self.lias = self.claude / "lias"
        self.database = self.lias / "database"
        self.db_file = self.database / "lias.sqlite3"
        self.backups = self.lias / "backups"
        self.benchmarks = self.lias / "benchmarks"
        self.manifests = self.lias / "automation" / "manifests"
        self.scripts = self.lias / "automation" / "scripts"
        self.generated = self.lias / "automation" / "generated"
        self.memory = self.claude / "memory"

    @property
    def project_id(self):
        return hashlib.sha256(os.path.normcase(str(self.root)).encode()).hexdigest()[:16]


def default_paths():
    """Project root = parent of the .claude directory that contains this code."""
    claude = Path(__file__).resolve().parents[2]
    if claude.name != ".claude":
        raise RuntimeError(f"LIAS must live in <project>/.claude/lias (found {claude})")
    return Paths(claude.parent)


def backup(paths, tag):
    if not paths.db_file.exists():
        return None
    paths.backups.mkdir(parents=True, exist_ok=True)
    dest = paths.backups / f"lias-{tag}-{time.strftime('%Y%m%d-%H%M%S')}.sqlite3"
    shutil.copy2(paths.db_file, dest)
    return dest


def connect(paths, rebind=False):
    paths.database.mkdir(parents=True, exist_ok=True)
    existed = paths.db_file.exists()
    conn = sqlite3.connect(paths.db_file, timeout=10)
    conn.row_factory = sqlite3.Row
    # DELETE journal (not WAL): friendlier to OneDrive-synced folders.
    conn.execute("PRAGMA journal_mode=DELETE")
    version = conn.execute("PRAGMA user_version").fetchone()[0]
    if version > len(MIGRATIONS):
        conn.close()
        raise RuntimeError(f"database schema {version} is newer than this LIAS ({len(MIGRATIONS)})")
    if version < len(MIGRATIONS):
        conn.close()
        if existed and version > 0:
            backup(paths, f"pre-migrate-v{version}")
        conn = sqlite3.connect(paths.db_file, timeout=10)
        conn.row_factory = sqlite3.Row
        try:
            for i in range(version, len(MIGRATIONS)):
                conn.executescript("BEGIN;" + MIGRATIONS[i] + f"PRAGMA user_version={i + 1};COMMIT;")
        except Exception:
            conn.close()
            raise
    row = conn.execute("SELECT value FROM meta WHERE key='project_id'").fetchone()
    if row is None:
        conn.execute("INSERT INTO meta VALUES('project_id',?)", (paths.project_id,))
        conn.execute("INSERT INTO meta VALUES('project_root',?)", (str(paths.root),))
        conn.commit()
    elif row[0] != paths.project_id:
        if not rebind:
            conn.close()
            raise ProjectMismatch(
                f"database belongs to another project path (id {row[0]}, this is {paths.project_id}); "
                "the project was copied or moved. Re-run with --rebind to adopt it.")
        conn.execute("UPDATE meta SET value=? WHERE key='project_id'", (paths.project_id,))
        conn.execute("UPDATE meta SET value=? WHERE key='project_root'", (str(paths.root),))
        conn.execute("UPDATE knowledge SET project_id=?", (paths.project_id,))
        conn.commit()
    return conn
