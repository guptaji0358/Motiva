"""Generation lifecycle tests. Run: python .claude/lias/tests/test_generation.py"""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.core import db, generation, runner  # noqa: E402
from lias.core.registry import Registry  # noqa: E402

GOOD = "import json,sys\njson.loads(sys.stdin.read())\nprint(json.dumps({'ok': True}))\n"


def manifest(aid, desc="count widgets in the project", **kw):
    m = {"id": aid, "name": aid, "description": desc, "entry": "script.py", "version": "1.0.0", "risk": 0,
         "timeout": 5, "status": "inactive", "args": {},
         "tests": [{"fixtures": {}, "args": {}, "expect": {"ok": True}}]}
    m.update(kw)
    return m


class GenerationTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lias-gen-t-"))
        self.paths = db.Paths(self.root)
        for d in (self.paths.scripts, self.paths.manifests, self.paths.generated):
            d.mkdir(parents=True)
        self.conn = db.connect(self.paths)

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def cand(self, aid, code=GOOD, **kw):
        d = self.paths.generated / aid
        d.mkdir(parents=True, exist_ok=True)
        (d / "script.py").write_text(code)
        (d / "manifest.json").write_text(json.dumps(manifest(aid, **kw)))
        return d

    def test_static_check_rejections(self):
        bad = ["import subprocess", "import socket", "import requests", "eval('1')", "exec('1')",
               "import os\nos.system('x')", "open('f','w')", "from pathlib import Path\nPath('a').write_text('x')",
               "import os\nos.remove('x')", "__import__('os')", "def f(:"]
        for src in bad:
            self.assertTrue(generation.static_check(src), src)
        self.assertEqual(generation.static_check("import os\nx='a b'.replace(' ','')\nopen(os.sep).read()"), [])

    def test_full_lifecycle(self):
        self.cand("widget-count")
        r = generation.propose(self.paths, self.conn, "widget-count")
        self.assertTrue(r["passed"], r)
        self.assertTrue(generation.promote(self.paths, self.conn, "widget-count")["promoted"])
        reg = Registry(self.paths, self.conn)
        env = runner.run(self.paths, self.conn, reg, "widget-count")
        self.assertEqual(env["status"], "success")
        self.assertTrue(generation.retire(self.paths, self.conn, "widget-count")["retired"])
        self.assertEqual(runner.run(self.paths, self.conn, reg, "widget-count")["error"]["code"], "NOT_FOUND")

    def test_promote_requires_passing_report(self):
        self.cand("widget-count")
        self.assertFalse(generation.promote(self.paths, self.conn, "widget-count")["promoted"])
        self.cand("fail-case", tests=[{"fixtures": {}, "args": {}, "expect": {"ok": False}}])
        self.assertFalse(generation.propose(self.paths, self.conn, "fail-case")["passed"])
        self.assertFalse(generation.promote(self.paths, self.conn, "fail-case")["promoted"])

    def test_edit_after_validation_blocks_promotion(self):
        d = self.cand("widget-count")
        generation.propose(self.paths, self.conn, "widget-count")
        (d / "script.py").write_text(GOOD + "# tampered\n")
        r = generation.promote(self.paths, self.conn, "widget-count")
        self.assertFalse(r["promoted"])
        self.assertIn("changed", r["reason"])

    def test_unsafe_script_never_tested_or_promoted(self):
        self.cand("evil-one", code="import subprocess\nprint('{}')")
        r = generation.propose(self.paths, self.conn, "evil-one")
        self.assertFalse(r["passed"])
        self.assertNotIn("tests", r["stages"])
        self.assertFalse(generation.promote(self.paths, self.conn, "evil-one")["promoted"])

    def test_risky_or_untested_rejected(self):
        self.cand("risky-one", risk=1)
        self.assertFalse(generation.propose(self.paths, self.conn, "risky-one")["passed"])
        self.cand("no-tests", tests=[])
        self.assertFalse(generation.propose(self.paths, self.conn, "no-tests")["passed"])

    def test_duplicate_detected(self):
        self.cand("widget-count")
        generation.propose(self.paths, self.conn, "widget-count")
        generation.promote(self.paths, self.conn, "widget-count")
        self.cand("widget-total", desc="Count widgets in the project")
        r = generation.propose(self.paths, self.conn, "widget-total")
        self.assertFalse(r["passed"])
        self.assertIn("similar", r["stages"]["duplicate"])

    def test_repromote_keeps_backup(self):
        d = self.cand("widget-count")
        generation.propose(self.paths, self.conn, "widget-count")
        generation.promote(self.paths, self.conn, "widget-count")
        (d / "manifest.json").write_text(json.dumps(manifest("widget-count", version="1.1.0")))
        generation.propose(self.paths, self.conn, "widget-count")
        generation.promote(self.paths, self.conn, "widget-count")
        self.assertTrue(list(self.paths.backups.glob("widget-count-*.json")))

    def test_hand_written_automation_cannot_be_retired(self):
        (self.paths.scripts / "hand.py").write_text(GOOD)
        m = manifest("hand-made", entry="hand.py")
        m.pop("tests")
        (self.paths.manifests / "hand-made.json").write_text(json.dumps(m))
        self.assertFalse(generation.retire(self.paths, self.conn, "hand-made")["retired"])

    def test_fixture_path_escape_fails_test(self):
        self.cand("escape-one", tests=[{"fixtures": {"../x.txt": "y"}, "args": {}, "expect": {"ok": True}}])
        self.assertFalse(generation.propose(self.paths, self.conn, "escape-one")["passed"])
        self.assertFalse((self.root.parent / "x.txt").exists())


if __name__ == "__main__":
    unittest.main()
