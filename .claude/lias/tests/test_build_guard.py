"""Build/installer safety tests (temp dirs only). Run: python .claude/lias/tests/test_build_guard.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
SCRIPTS = os.path.join(REPO, ".claude", "lias", "automation", "scripts")
sys.path.insert(0, SCRIPTS)
import _build_guard as guard  # noqa: E402


class SafeBuildTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="lias-guard-")
        self.build = os.path.join(self.tmp, "build")
        os.makedirs(self.build)
        self.child = None

    def tearDown(self):
        if self.child:  # only the process this test started
            self.child.kill()
            self.child.wait()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def cache(self, source):
        with open(os.path.join(self.build, "CMakeCache.txt"), "w") as f:
            f.write("CMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % source.replace("\\", "/"))

    def start_from_build_dir(self):
        exe = os.path.join(self.build, "lias_guard_dummy.exe")
        shutil.copy(os.path.join(os.environ["SystemRoot"], "System32", "PING.EXE"), exe)
        self.child = subprocess.Popen([exe, "-n", "60", "127.0.0.1"], stdout=subprocess.DEVNULL)

    def safe_build(self, procs=()):
        with mock.patch.object(guard, "list_processes", return_value=list(procs)), \
                mock.patch.object(guard, "run_capture", return_value=(0, "")) as ran, \
                mock.patch.object(guard.shutil, "which", return_value="cmake"):
            return guard.safe_build(self.tmp, "build", {"PATH": ""}), ran

    def test_unconfigured_dir_refused(self):
        r, ran = self.safe_build()
        self.assertIn("not configured", r["refused"])
        ran.assert_not_called()

    def test_other_source_tree_refused(self):
        self.cache(os.path.join(self.tmp, "elsewhere"))
        r, ran = self.safe_build()
        self.assertIn("another source tree", r["refused"])
        ran.assert_not_called()

    def test_process_in_build_dir_refused(self):
        self.cache(self.tmp)
        exe = os.path.join(self.build, "sub", "Motiva.exe")
        r, ran = self.safe_build([{"ProcessId": 7, "Name": "Motiva.exe", "ExecutablePath": exe}])
        self.assertEqual(r["running"][0]["pid"], 7)
        ran.assert_not_called()

    def test_unknown_motiva_path_blocks_but_sibling_dir_does_not(self):
        self.cache(self.tmp)
        r, _ = self.safe_build([{"ProcessId": 1, "Name": "Motiva.exe", "ExecutablePath": None}])
        self.assertIn("refused", r)
        other = os.path.join(self.tmp, "build-other", "Motiva.exe")
        r, ran = self.safe_build([{"ProcessId": 2, "Name": "Motiva.exe", "ExecutablePath": other}])
        self.assertEqual(r["exit_code"], 0)
        ran.assert_called_once()

    def test_process_check_failure_refuses(self):
        self.cache(self.tmp)
        with mock.patch.object(guard, "list_processes", side_effect=RuntimeError("boom")), \
                mock.patch.object(guard, "run_capture") as ran:
            r = guard.safe_build(self.tmp, "build", {"PATH": ""})
        self.assertIn("process check failed", r["refused"])
        ran.assert_not_called()

    def test_real_process_is_detected_and_left_running(self):
        self.cache(self.tmp)
        self.start_from_build_dir()
        r = guard.safe_build(self.tmp, "build", {"PATH": ""})
        self.assertEqual([h["pid"] for h in r["running"]], [self.child.pid])
        self.assertIsNone(self.child.poll())

    def test_installer_script_refuses_and_writes_nothing(self):
        self.cache(REPO)
        self.start_from_build_dir()
        out = os.path.join(self.tmp, "out")
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                            os.path.join(REPO, "scripts", "installer", "build-installer.ps1"),
                            "-BuildDir", self.build, "-OutputDir", out], capture_output=True, text=True, timeout=90)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertTrue(r.stdout.startswith("REFUSED:"), r.stdout)
        self.assertIn("nothing was killed", r.stdout)
        self.assertFalse(os.path.exists(out))
        self.assertIsNone(self.child.poll())


class NoKillTests(unittest.TestCase):
    def test_no_process_killing_in_build_paths(self):
        for rel in (("build_compile.py",), ("precommit_check.py",), ("installer_build.py",), ("_build_guard.py",)):
            with open(os.path.join(SCRIPTS, *rel), encoding="utf-8") as f:
                self.assertNotRegex(f.read(), r"(?i)taskkill|Stop-Process")
        with open(os.path.join(REPO, "scripts", "installer", "build-installer.ps1"), encoding="utf-8") as f:
            self.assertNotRegex(f.read(), r"(?i)taskkill|Stop-Process|\.Kill\(")


if __name__ == "__main__":
    unittest.main()
