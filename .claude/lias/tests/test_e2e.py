"""End-to-end (Phase 8): two real projects, each with its own copy of LIAS, driven only through the CLI in
separate processes. Run: python .claude/lias/tests/test_e2e.py"""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

LIAS_SRC = Path(__file__).resolve().parents[1]
IGNORE = shutil.ignore_patterns("__pycache__", "database", "backups", "benchmarks", "tests", "generated")
CAND = {"id": "txt-count", "name": "Text file count", "description": "Count txt files in the project root",
        "entry": "script.py", "version": "1.0.0", "risk": 0, "timeout": 10, "status": "inactive", "args": {},
        "tests": [{"fixtures": {"a.txt": "x", "b.txt": "y"}, "args": {}, "expect": {"count": 2}}]}
CAND_SCRIPT = ("import json,os\nr=os.environ['LIAS_PROJECT_ROOT']\n"
               "print(json.dumps({'count': len([f for f in os.listdir(r) if f.endswith('.txt')])}))\n")


def make_project(prefix):
    root = Path(tempfile.mkdtemp(prefix=prefix))
    shutil.copytree(LIAS_SRC, root / ".claude" / "lias", ignore=IGNORE)
    return root


def cli(root, *args, check=True):
    p = subprocess.run([sys.executable, str(root / ".claude" / "lias" / "cli.py"), *args],
                       capture_output=True, text=True, cwd=root, timeout=60)
    if check and p.returncode != 0:
        raise AssertionError(f"{args} -> rc {p.returncode}\n{p.stdout}\n{p.stderr}")
    return p.returncode, (json.loads(p.stdout) if p.stdout.strip() else None)


class EndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.a, cls.b = make_project("lias-e2e-a-"), make_project("lias-e2e-b-")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.a, ignore_errors=True)
        shutil.rmtree(cls.b, ignore_errors=True)

    def test_scenarios(self):
        a, b = self.a, self.b
        # A: persistent knowledge across separate processes (separate "sessions")
        cli(a, "knowledge", "add", "architecture_decision", "Renderer", "DirectComposition for desktop attach",
            "--source", "e2e", "--status", "verified")
        _, hits = cli(a, "knowledge", "search", "how is desktop attach rendered")
        self.assertEqual(hits[0]["status"], "verified")
        # E: isolation - B has its own database and cannot see A's record
        _, hits_b = cli(b, "knowledge", "search", "DirectComposition")
        self.assertEqual(hits_b, [])
        self.assertNotEqual(cli(a, "status")[1]["project_id"], cli(b, "status")[1]["project_id"])
        # C: LIAS creates, validates, registers and reuses an automation
        gen = a / ".claude/lias/automation/generated/txt-count"
        gen.mkdir(parents=True)
        (gen / "manifest.json").write_text(json.dumps(CAND))
        (gen / "script.py").write_text(CAND_SCRIPT)
        self.assertEqual(cli(a, "automation", "run", "txt-count", check=False)[0], 1)  # not registered yet
        self.assertTrue(cli(a, "automation", "propose", "txt-count")[1]["passed"])
        self.assertTrue(cli(a, "automation", "promote", "txt-count")[1]["promoted"])
        (a / "one.txt").write_text("1")
        (a / "two.txt").write_text("2")
        self.assertEqual(cli(a, "automation", "run", "txt-count")[1]["result"]["count"], 2)  # later reuse
        self.assertEqual(cli(b, "automation", "run", "txt-count", check=False)[0], 1)  # not leaked to B
        # F: permission - risk-2 automation is denied without authorization
        m = json.loads((a / ".claude/lias/automation/manifests/txt-count.json").read_text())
        m.update(id="risky-op", risk=2)
        (a / ".claude/lias/automation/manifests/risky-op.json").write_text(json.dumps(m))
        rc, env = cli(a, "automation", "run", "risky-op", check=False)
        self.assertEqual((rc, env["status"]), (1, "denied"))
        # D: controlled failure -> quarantine, then release
        s = a / ".claude/lias/automation/scripts"
        (s / "boom.py").write_text("import sys\nsys.exit(9)\n")
        (a / ".claude/lias/automation/manifests/boom-op.json").write_text(
            json.dumps(dict(m, id="boom-op", risk=0, entry="boom.py", status="active")))
        for _ in range(3):
            env = cli(a, "automation", "run", "boom-op", check=False)[1]
        self.assertIn("quarantined", env["warnings"][0])
        self.assertEqual(cli(a, "automation", "run", "boom-op", check=False)[1]["error"]["code"], "NOT_ACTIVE")
        self.assertEqual(cli(a, "doctor", check=False)[0], 1)  # doctor flags the quarantined automation
        # recovery: backup, damage, restore
        name = cli(a, "backup", "create")[1]["created"]
        (a / ".claude/lias/database/lias.sqlite3").write_bytes(b"corrupted" * 100)
        self.assertNotEqual(cli(a, "status", check=False)[0], 0)
        self.assertTrue(cli(a, "backup", "restore", name)[1]["restored"])
        self.assertEqual(cli(a, "knowledge", "search", "DirectComposition")[1][0]["title"], "Renderer")


if __name__ == "__main__":
    unittest.main()
