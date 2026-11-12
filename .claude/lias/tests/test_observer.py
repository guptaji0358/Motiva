"""Observer and hook tests. Run: python .claude/lias/tests/test_observer.py"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.core import db  # noqa: E402
from lias.learning import observer  # noqa: E402

HOOK = Path(__file__).resolve().parents[1] / "hooks" / "post_tool_use.py"


class ObserverTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="lias-obs-"))
        self.conn = db.connect(db.Paths(self.root))

    def tearDown(self):
        self.conn.close()
        shutil.rmtree(self.root, ignore_errors=True)

    def test_threshold_and_normalization(self):
        for c in ("cmake  --build build", "cmake --build build ", "cmake --build build"):
            observer.record(self.conn, c)
        observer.record(self.conn, "npm run lint:all")
        found = observer.candidates(self.conn)
        self.assertEqual([f["command"] for f in found], ["cmake --build build"])
        self.assertEqual(found[0]["count"], 3)

    def test_ignored_and_secret_commands_not_recorded(self):
        self.assertFalse(observer.record(self.conn, "ls -la"))
        self.assertFalse(observer.record(self.conn, "python .claude/lias/cli.py doctor"))
        self.assertFalse(observer.record(self.conn, "curl -H 'password=hunter2222' http://x"))
        self.assertEqual(self.conn.execute("SELECT COUNT(*) FROM observed").fetchone()[0], 0)

    def test_dismiss(self):
        for _ in range(3):
            observer.record(self.conn, "run-the-thing --flag")
        observer.dismiss(self.conn, "run-the-thing --flag")
        self.assertEqual(observer.candidates(self.conn), [])


class HookTests(unittest.TestCase):
    def run_hook(self, payload, **env):
        return subprocess.run([sys.executable, str(HOOK)], input=payload, capture_output=True, text=True,
                              timeout=15, env=dict(os.environ, **env))

    def test_garbage_input_is_silent_and_succeeds(self):
        for payload in ("", "not json", "[]", json.dumps({"tool_input": {"command": 5}})):
            r = self.run_hook(payload)
            self.assertEqual((r.returncode, r.stdout, r.stderr), (0, "", ""))

    def test_recursion_guard(self):
        r = self.run_hook(json.dumps({"tool_input": {"command": "some long command"}}), LIAS_HOOK_ACTIVE="1")
        self.assertEqual(r.returncode, 0)


if __name__ == "__main__":
    unittest.main()
