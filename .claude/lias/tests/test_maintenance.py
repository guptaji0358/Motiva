"""Backup/restore tests. Run: python .claude/lias/tests/test_maintenance.py"""
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.core import db, maintenance  # noqa: E402
from lias.core.knowledge import Knowledge  # noqa: E402


class MaintenanceTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lias-mnt-"))
        self.paths = db.Paths(self.root)
        self.conn = db.connect(self.paths)
        self.kn = Knowledge(self.conn, self.paths.project_id)

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def test_backup_and_restore_roundtrip(self):
        self.kn.add("project_fact", "keep me", "zebra fact", "t", status="verified")
        name = maintenance.backup_create(self.paths, self.conn)
        self.kn.add("project_fact", "added later", "giraffe fact", "t")
        self.conn.close()
        r = maintenance.backup_restore(self.paths, name)
        self.assertTrue(r["restored"])
        self.conn = db.connect(self.paths)
        kn = Knowledge(self.conn, self.paths.project_id)
        self.assertEqual(len(kn.search("zebra")), 1)
        self.assertEqual(kn.search("giraffe"), [])
        self.assertTrue((self.paths.backups / r["previous_saved_as"]).exists())

    def test_restore_rejects_bad_inputs(self):
        self.conn.close()
        self.assertFalse(maintenance.backup_restore(self.paths, "nope.sqlite3")["restored"])
        self.paths.backups.mkdir(parents=True, exist_ok=True)
        (self.paths.backups / "lias-garbage.sqlite3").write_bytes(b"not a database" * 50)
        self.assertFalse(maintenance.backup_restore(self.paths, "lias-garbage.sqlite3")["restored"])
        self.assertFalse(maintenance.backup_restore(self.paths, "../../evil.sqlite3")["restored"])
        self.conn = db.connect(self.paths)

    def test_restore_rejects_other_projects_backup(self):
        other = Path(tempfile.mkdtemp(prefix="lias-other-"))
        try:
            p2 = db.Paths(other)
            c2 = db.connect(p2)
            name = maintenance.backup_create(p2, c2)
            c2.close()
            self.conn.close()
            self.paths.backups.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p2.backups / name, self.paths.backups / name)
            r = maintenance.backup_restore(self.paths, name)
            self.assertFalse(r["restored"])
            self.assertIn("different project", r["reason"])
            self.conn = db.connect(self.paths)
        finally:
            shutil.rmtree(other, ignore_errors=True)

    def test_backup_retention(self):
        for i in range(maintenance.KEEP_BACKUPS + 3):
            (self.paths.backups).mkdir(parents=True, exist_ok=True)
            (self.paths.backups / f"lias-manual-2000010{i % 10}-0000{i:02d}.sqlite3").write_bytes(b"x")
        maintenance.backup_create(self.paths, self.conn)
        self.assertLessEqual(len(list(self.paths.backups.glob("lias-manual-*"))), maintenance.KEEP_BACKUPS)


if __name__ == "__main__":
    unittest.main()
