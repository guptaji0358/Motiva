"""Runner tests against a throwaway project. Run: python .claude/lias/tests/test_runner.py"""
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.core import db, runner  # noqa: E402
from lias.core.registry import Registry  # noqa: E402

OK = "import json,sys\njson.loads(sys.stdin.read())\nprint(json.dumps({'ok':True}))\n"
FAIL = "import sys\nsys.exit(3)\n"
SLEEP = "import time\ntime.sleep(30)\n"
NOJSON = "print('hello')\n"


def manifest(aid, entry, **kw):
    m = {"id": aid, "name": aid, "description": "t", "entry": entry, "version": "1.0.0", "risk": 0,
         "timeout": 2, "status": "active", "args": {"n": {"type": "integer", "default": 1}}}
    m.update(kw)
    return m


class RunnerTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lias-run-"))
        self.paths = db.Paths(self.root)
        self.paths.scripts.mkdir(parents=True)
        self.paths.manifests.mkdir(parents=True)
        self.conn = db.connect(self.paths)
        self.reg = Registry(self.paths, self.conn)

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def add(self, aid, code, **kw):
        (self.paths.scripts / f"{aid}.py").write_text(code)
        (self.paths.manifests / f"{aid}.json").write_text(json.dumps(manifest(aid, f"{aid}.py", **kw)))

    def run_(self, aid, args=None, **kw):
        return runner.run(self.paths, self.conn, self.reg, aid, args, **kw)

    def test_success(self):
        self.add("good", OK)
        r = self.run_("good", {"n": 5})
        self.assertEqual(r["status"], "success")
        self.assertEqual(r["result"], {"ok": True})

    def test_bad_args_rejected(self):
        self.add("good", OK)
        for args in ({"n": "x"}, {"zzz": 1}, {"n": True}):
            self.assertEqual(self.run_("good", args)["error"]["code"], "BAD_ARGS")

    def test_unknown_automation(self):
        self.assertEqual(self.run_("nope")["error"]["code"], "NOT_FOUND")

    def test_timeout(self):
        self.add("slow", SLEEP, timeout=1)
        self.assertEqual(self.run_("slow")["error"]["code"], "TIMEOUT")

    def test_non_json_output_is_failure(self):
        self.add("bad", NOJSON)
        self.assertEqual(self.run_("bad")["error"]["code"], "SCRIPT_FAILED")

    def test_risk_tier_denied(self):
        self.add("risky", OK, risk=2)
        self.assertEqual(self.run_("risky")["status"], "denied")
        self.assertEqual(self.run_("risky", allow_risk=2)["status"], "success")

    def test_quarantine_after_repeated_failures_and_release(self):
        self.add("flaky", FAIL)
        for _ in range(runner.QUARANTINE_AFTER):
            r = self.run_("flaky")
        self.assertIn("quarantined", r["warnings"][0])
        self.assertEqual(self.run_("flaky")["error"]["code"], "NOT_ACTIVE")
        self.reg.release("flaky")
        self.assertEqual(self.run_("flaky")["error"]["code"], "SCRIPT_FAILED")

    def test_cache_hit_and_invalidation(self):
        self.add("cached-op", OK, cache={"policy": "fingerprint", "extensions": [".txt"], "ttl": 60})
        f = self.root / "a.txt"
        f.write_text("one")
        self.assertFalse(self.run_("cached-op")["cached"])
        self.assertTrue(self.run_("cached-op")["cached"])
        f.write_text("two-longer")
        self.assertFalse(self.run_("cached-op")["cached"])
        self.assertFalse(self.run_("cached-op", use_cache=False)["cached"])

    def test_failures_not_cached(self):
        self.add("fails", FAIL, cache={"policy": "fingerprint", "extensions": [".txt"]})
        self.assertEqual(self.run_("fails")["error"]["code"], "SCRIPT_FAILED")
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM cache").fetchone()[0], 0)

    def test_bad_manifest_does_not_break_registry(self):
        self.add("good", OK)
        (self.paths.manifests / "broken.json").write_text("{not json")
        good, bad = self.reg.load()
        self.assertIn("good", good)
        self.assertIn("broken.json", bad)

    def test_args_not_shell_interpreted(self):
        self.add("echo", "import json,sys\nprint(json.dumps({'got': json.loads(sys.stdin.read())['s']}))\n",
                 args={"s": {"type": "string", "required": True}})
        evil = "x & echo pwned > pwned.txt; $(calc)"
        self.assertEqual(self.run_("echo", {"s": evil})["result"]["got"], evil)
        self.assertFalse((self.root / "pwned.txt").exists())


if __name__ == "__main__":
    unittest.main()
