"""Core tests: isolation, knowledge, migration, registry validation. Run: python -m unittest discover .claude/lias/tests"""
import json
import shutil
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.core import db, diagnostics  # noqa: E402
from lias.core.knowledge import Knowledge, SecretRejected  # noqa: E402
from lias.core.registry import ManifestError, validate_manifest  # noqa: E402

REAL_SCRIPTS = Path(__file__).resolve().parents[1] / "automation"


def make_project():
    root = Path(tempfile.mkdtemp(prefix="lias-test-"))
    paths = db.Paths(root)
    conn = db.connect(paths)
    return root, paths, conn, Knowledge(conn, paths.project_id)


class CoreTests(unittest.TestCase):
    def setUp(self):
        self.root, self.paths, self.conn, self.kn = make_project()

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def test_paths_are_project_local(self):
        self.assertTrue(str(self.paths.db_file).startswith(str(self.root)))
        self.assertTrue(self.paths.db_file.exists())

    def test_persistence_across_connections(self):
        self.kn.add("project_fact", "Build tool", "Ninja with MinGW", "test", status="verified")
        self.conn.close()
        self.conn = db.connect(self.paths)
        kn = Knowledge(self.conn, self.paths.project_id)
        hits = kn.search("what build tool ninja")
        self.assertEqual(hits[0]["title"], "Build tool")
        self.assertEqual(hits[0]["status"], "verified")

    def test_project_isolation(self):
        self.kn.add("project_fact", "Only here", "alpha secretless fact", "test", status="verified")
        root2, paths2, conn2, kn2 = make_project()
        try:
            self.assertEqual(kn2.search("alpha"), [])
            self.assertNotEqual(self.paths.project_id, paths2.project_id)
        finally:
            conn2.close()
            shutil.rmtree(root2, ignore_errors=True)

    def test_moved_project_detected(self):
        self.conn.close()
        other = Path(tempfile.mkdtemp(prefix="lias-moved-"))
        try:
            (other / ".claude" / "lias").mkdir(parents=True)
            shutil.copytree(self.paths.database, other / ".claude" / "lias" / "database")
            with self.assertRaises(db.ProjectMismatch):
                db.connect(db.Paths(other))
            c = db.connect(db.Paths(other), rebind=True)
            c.close()
        finally:
            shutil.rmtree(other, ignore_errors=True)
            self.conn = db.connect(self.paths)

    def test_secrets_rejected(self):
        for text in ("password = hunter2222", "key ghp_abcdefghijklmnopqrstuvwx", "api_key: abcd1234"):
            with self.assertRaises(SecretRejected):
                self.kn.add("project_fact", "t", text, "test")
        self.assertEqual(self.kn.list(), [])

    def test_contradiction_not_silently_overwritten(self):
        self.kn.add("build_config", "Qt version", "6.12.0", "test", status="verified")
        r = self.kn.add("build_config", "Qt version", "6.11.1", "claim", status="unverified")
        self.assertEqual(r["result"], "conflict")
        self.assertEqual(self.kn.search("Qt version")[0]["content"], "6.12.0")
        r = self.kn.add("build_config", "Qt version", "6.13.0", "test", status="verified", supersede=True)
        self.assertEqual(r["result"], "updated")
        self.assertEqual(r["version"], 2)

    def test_expiry_and_invalidation_hidden(self):
        self.kn.add("temporary", "scratch", "ephemeral note about zebra", "test", expires_days=1)
        self.assertEqual(len(self.kn.search("zebra")), 1)
        self.conn.execute("UPDATE knowledge SET expires_at=1")
        self.assertEqual(self.kn.search("zebra"), [])
        self.assertEqual(self.kn.expire_due(), 1)
        rid = self.kn.add("project_fact", "gone", "giraffe fact", "test")["id"]
        self.kn.invalidate(rid, "wrong")
        self.assertEqual(self.kn.search("giraffe"), [])

    def test_fts_consistency_after_delete(self):
        rid = self.kn.add("project_fact", "x", "unique-token quokka", "test")["id"]
        self.kn.delete(rid)
        self.assertTrue(diagnostics.doctor(self.paths, self.conn)["ok"])

    def test_context_budget(self):
        for i in range(10):
            self.kn.add("project_fact", f"item {i}", "lorem ipsum " * 200, "test")
        res = self.kn.search("lorem ipsum", limit=5, max_chars=600)
        self.assertLessEqual(sum(len(r["content"]) for r in res), 600 + 5 * 3)

    def test_markdown_index_incremental(self):
        self.paths.memory.mkdir(parents=True)
        (self.paths.memory / "a.md").write_text("# A\n\n## Sec\n" + "wallpaper rotation details " * 5)
        s1 = self.kn.index_markdown(self.paths.memory)
        s2 = self.kn.index_markdown(self.paths.memory)
        self.assertGreater(s1["records"], 0)
        self.assertEqual(s2["skipped"], 1)
        self.assertEqual(self.kn.search("rotation")[0]["status"], "documented")

    def test_migration_backup_and_newer_schema_refused(self):
        self.conn.close()
        c = sqlite3.connect(self.paths.db_file)
        c.execute("PRAGMA user_version=99")
        c.commit()
        c.close()
        with self.assertRaises(RuntimeError):
            db.connect(self.paths)
        # restore a real v1 database and check it upgrades with a backup
        self.paths.db_file.unlink()
        c = sqlite3.connect(self.paths.db_file)
        c.executescript("BEGIN;" + db.MIGRATIONS[0] + "PRAGMA user_version=1;COMMIT;")
        c.close()
        self.conn = db.connect(self.paths)
        self.assertEqual(self.conn.execute("PRAGMA user_version").fetchone()[0], len(db.MIGRATIONS))
        self.conn.execute("SELECT * FROM observed")
        self.assertEqual(len(list(self.paths.backups.glob("lias-pre-migrate-v1-*"))), 1)

    def test_manifest_validation(self):
        good = json.loads((REAL_SCRIPTS / "manifests" / "source-stats.json").read_text())
        validate_manifest(good)
        for patch in ({"entry": "../evil.py"}, {"entry": "a\\b.py"}, {"risk": 9}, {"timeout": 0},
                      {"id": "Bad ID"}, {"status": "quarantined"}):
            with self.assertRaises(ManifestError):
                validate_manifest(dict(good, **patch))


if __name__ == "__main__":
    unittest.main()
