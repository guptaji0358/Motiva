"""git-review tests on throwaway repositories. Run: python .claude/lias/tests/test_git_review.py"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "automation" / "scripts" / "git_review.py"
FAKE_KEY = "ghp_" + "A1b2" * 8  # built at run time so this file holds no key-shaped text


class GitReview(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="review-"))
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.invalid")
        self.git("config", "user.name", "t")
        self.git("config", "core.autocrlf", "false")
        self.write("scripts/tests/CMakeLists.txt",
                   "".join("qt_add_executable(%sTests x.cpp)\n" % n for n in "ABCD"))
        self.write("Assets/icons/old.svg", "<svg/>")
        self.write("app.qrc", "<file>Assets/icons/old.svg</file>\n")
        self.commit("base")
        self.git("switch", "-q", "-c", "work")

    def tearDown(self):
        def writable(func, path, _):
            os.chmod(path, 0o700)
            func(path)
        shutil.rmtree(self.root, onerror=writable)

    def git(self, *args):
        return subprocess.run(["git"] + list(args), cwd=self.root, check=True, capture_output=True, text=True).stdout

    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8", newline="\n")

    def commit(self, msg):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", msg)

    def review(self, **args):
        p = subprocess.run([sys.executable, "-I", str(SCRIPT)], input=json.dumps(args), capture_output=True,
                           text=True, encoding="utf-8", env=dict(os.environ, LIAS_PROJECT_ROOT=str(self.root)))
        self.assertEqual(p.returncode, 0, p.stderr)
        return json.loads(p.stdout.strip().splitlines()[-1])

    def test_clean_repo(self):
        r = self.review()
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["working_tree"], {"staged": [], "unstaged": [], "untracked": [], "conflicted": []})
        self.assertEqual((r["range"]["files"], r["secret_hits"], r["missing_qrc_targets"], r["stale_notes"]),
                         (0, [], [], []))
        self.assertIsNone(r["upstream_ahead_behind"])

    def test_pending_files_are_grouped_and_secrets_held_back_without_value(self):
        self.write("scripts/src/a.cpp", "int a;\n")
        self.write("scripts/deploy/new file.ps1", "x\n")
        self.write("config.txt", "line\nkey " + FAKE_KEY + "\n")
        self.write("dev.pem", "not a real key\n")
        r = self.review()
        self.assertEqual(r["secret_hits"], ["config.txt:2"])
        self.assertNotIn(FAKE_KEY, json.dumps(r))
        self.assertEqual(r["unwanted_files"], ["dev.pem"])
        self.assertEqual(r["held_back"], ["config.txt", "dev.pem"])
        self.assertEqual(r["candidates_to_commit"], ["scripts/deploy/new file.ps1", "scripts/src/a.cpp"])
        self.assertIn("pending scripts/src/a.cpp", r["groups"]["source"])
        self.assertIn("pending scripts/deploy/new file.ps1", r["groups"]["deploy-installer"])

    def test_deleted_asset_still_referenced(self):
        (self.root / "Assets/icons/old.svg").unlink()
        self.commit("remove icon")
        r = self.review(base="main")
        self.assertEqual(r["range"]["by_status"], {"D": 1})
        self.assertIn("app.qrc -> Assets/icons/old.svg (not tracked)", r["missing_qrc_targets"])
        self.assertEqual(r["deleted_references"], ["app.qrc mentions deleted Assets/icons/old.svg"])

    def test_stale_agent_notes(self):
        self.write(".claude/agents/a.md", "runs the three test suites\nRun ATests, BTests and CTests.\n")
        self.write(".claude/agents/b.md", "runs four suites: ATests BTests CTests DTests\n")
        self.write(".claude/memory/big.md", "x\n" * 201)
        self.commit("notes")
        notes = self.review()["stale_notes"]
        self.assertEqual(notes, [".claude/agents/a.md:1: says 3 suites, CMake defines 4",
                                 ".claude/agents/a.md:2: test list omits DTests",
                                 ".claude/memory/big.md: 201 lines (limit 200)"])

    def test_conflict_marker_and_bad_base(self):
        self.write("x.txt", "<<<<<<< HEAD\n")
        self.assertEqual(self.review()["conflict_markers"], ["x.txt:1"])
        for base in ("nope", "--all"):
            self.assertFalse(self.review(base=base)["ok"])

    def test_nothing_in_the_repository_changes(self):
        self.write("staged.txt", "1\n")
        self.git("add", "staged.txt")
        self.write("loose.txt", "2\n")

        def snapshot():
            return {str(p): (p.stat().st_mtime_ns, p.read_bytes()) for p in self.root.rglob("*") if p.is_file()}
        before = snapshot()
        r = self.review(report=True)
        self.assertEqual(snapshot(), before)
        report = Path(r["report_path"])
        self.assertNotIn(self.root, report.parents)
        self.assertIn("staged.txt", report.read_text(encoding="utf-8"))
        report.unlink()


if __name__ == "__main__":
    unittest.main()
