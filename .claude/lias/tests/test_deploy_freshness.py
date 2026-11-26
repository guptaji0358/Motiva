"""Per-target skip_build freshness tests on a throwaway tree. Run: python .claude/lias/tests/test_deploy_freshness.py
Never touches the real project, out/ or any deployment: everything lives in a temp dir."""
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parents[1] / "automation" / "scripts"
sys.path.insert(0, str(SCRIPTS))

import _deploy_fresh as F  # noqa: E402
import _deploy_qt  # noqa: E402

T0 = 1_700_000_000  # all inputs; artifacts are built later, edits later still
BUILT, EDIT = T0 + 1000, T0 + 2000

# Mirrors the real CMake/Ninja shape: autogen is order-only on the other exe; Motiva is order-only on the host;
# an unrelated phony chain (..._timestamp_deps) lists the host exe as an explicit input.
NINJA = r"""cmake_ninja_workdir =
rule CXX
  command = x
build CMakeFiles/Motiva.dir/scripts/src/a.cpp.obj: CXX {root}/scripts/src/a.cpp || cmake_object_order_depends_target_Motiva
  INCLUDES = -I{root}/scripts/include -isystem {qt}/include -I{root}/out/build/Motiva_autogen/include
build CMakeFiles/Motiva.dir/res.cpp.obj: CXX Motiva_autogen/qrc.cpp || cmake_object_order_depends_target_Motiva
build Motiva_autogen/qrc.cpp | ${{cmake_ninja_workdir}}Motiva_autogen/qrc.cpp: CUSTOM {root}/scripts/resources/app.qrc {root}/Assets/icon.svg
build Motiva.exe: LINK CMakeFiles/Motiva.dir/scripts/src/a.cpp.obj CMakeFiles/Motiva.dir/res.cpp.obj {qt}/lib/libQt6Core.a || MotivaBrowserHost.exe
build cmake_object_order_depends_target_Motiva: phony || MotivaBrowserHost.exe Motiva_autogen_timestamp_deps
build Motiva_autogen_timestamp_deps: phony MotivaBrowserHost.exe
build CMakeFiles/MotivaBrowserHost.dir/host.cpp.obj: CXX {root}/scripts/browser/host.cpp || cmake_object_order_depends_target_MotivaBrowserHost
  INCLUDES = -I{root}/out/build/MotivaBrowserHost_autogen/include
build cmake_object_order_depends_target_MotivaBrowserHost: phony
build MotivaBrowserHost.exe: LINK CMakeFiles/MotivaBrowserHost.dir/host.cpp.obj
build build.ninja: RERUN_CMAKE | {root}/CMakeLists.txt {root}/scripts/extra.cmake {qt}/lib/cmake/Qt6Config.cmake
"""


def touch(p, t):
    Path(p).parent.mkdir(parents=True, exist_ok=True)
    if not Path(p).exists():
        Path(p).write_text("x")
    os.utime(p, (t, t))


class Fresh(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="fresh-"))
        self.root = self.tmp / "proj"
        self.qt = self.tmp / "qt"
        self.build = self.root / "out" / "build"
        self.write_ninja(NINJA)
        for p in ("scripts/src/a.cpp", "scripts/include/a.h", "scripts/browser/host.cpp", "scripts/resources/app.qrc",
                  "Assets/icon.svg", "CMakeLists.txt", "scripts/extra.cmake"):
            touch(self.root / p, T0)
        touch(self.qt / "lib" / "libQt6Core.a", T0)
        touch(self.qt / "lib" / "cmake" / "Qt6Config.cmake", T0)
        touch(self.build / "CMakeCache.txt", T0)
        touch(self.build / "build.ninja", T0)
        for e in ("Motiva.exe", "MotivaBrowserHost.exe"):
            touch(self.build / e, BUILT)
        self.arts = [str(self.build / "Motiva.exe"), str(self.build / "MotivaBrowserHost.exe")]

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def write_ninja(self, text):
        self.build.mkdir(parents=True, exist_ok=True)
        keep = (self.build / "build.ninja").stat().st_mtime if (self.build / "build.ninja").exists() else T0
        (self.build / "build.ninja").write_text(text.format(root=self.root.as_posix(), qt=self.qt.as_posix()))
        os.utime(self.build / "build.ninja", (keep, keep))

    def run_(self):
        r = F.freshness(str(self.root), str(self.build), self.arts)
        return r, {k: v["fresh"] for k, v in r["targets"].items()}

    def edit(self, rel):
        touch(self.root / rel, EDIT)

    # ---- the five required scenarios ----
    def test_neither_changed(self):
        r, t = self.run_()
        self.assertTrue(r["fresh"], r)
        self.assertEqual(t, {"Motiva.exe": True, "MotivaBrowserHost.exe": True})

    def test_only_motiva_inputs_changed(self):
        for rel in ("scripts/src/a.cpp", "scripts/include/a.h", "Assets/icon.svg"):  # source, header (-I sweep), qrc asset
            with self.subTest(rel=rel):
                touch(self.root / rel, T0)
                self.edit(rel)
                r, t = self.run_()
                self.assertFalse(r["fresh"])
                self.assertEqual(t, {"Motiva.exe": False, "MotivaBrowserHost.exe": True}, r)
                self.assertIn("Motiva.exe", r["reason"])
                self.assertNotIn("MotivaBrowserHost.exe", r["reason"])
                touch(self.root / rel, T0)

    def test_only_host_inputs_changed(self):
        self.edit("scripts/browser/host.cpp")
        r, t = self.run_()
        self.assertFalse(r["fresh"])
        self.assertEqual(t, {"Motiva.exe": True, "MotivaBrowserHost.exe": False}, r)  # order-only link must not leak

    def test_both_changed(self):
        self.edit("scripts/src/a.cpp")
        self.edit("scripts/browser/host.cpp")
        r, t = self.run_()
        self.assertEqual(t, {"Motiva.exe": False, "MotivaBrowserHost.exe": False})
        self.assertFalse(r["fresh"])

    # ---- shared / generated inputs ----
    def test_shared_config_affects_both(self):
        for rel in ("CMakeLists.txt", "scripts/extra.cmake", "out/build/CMakeCache.txt", "out/build/build.ninja"):
            with self.subTest(rel=rel):
                self.edit(rel)
                r, t = self.run_()
                self.assertEqual(t, {"Motiva.exe": False, "MotivaBrowserHost.exe": False}, r)
                touch(self.root / rel, T0)

    def test_toolchain_leaf_affects_target_that_uses_it(self):
        touch(self.qt / "lib" / "libQt6Core.a", EDIT)
        _, t = self.run_()
        self.assertEqual(t, {"Motiva.exe": False, "MotivaBrowserHost.exe": True})

    def test_input_as_new_as_artifact_is_stale(self):
        touch(self.root / "scripts/src/a.cpp", BUILT)
        _, t = self.run_()
        self.assertFalse(t["Motiva.exe"])

    def test_generated_outputs_are_not_inputs(self):  # autogen output newer than the exe is not a source change
        touch(self.build / "Motiva_autogen" / "qrc.cpp", EDIT)
        r, _ = self.run_()
        self.assertTrue(r["fresh"], r)

    def test_unrelated_file_is_ignored(self):
        touch(self.root / "scripts" / "docs" / "notes.cpp", EDIT)
        r, _ = self.run_()
        self.assertTrue(r["fresh"], r)

    # ---- missing / ambiguous dependency information: always NOT fresh, with an explanation ----
    def assert_unsafe(self, r, *words):
        self.assertFalse(r["fresh"], r)
        for w in words:
            self.assertIn(w, r["reason"])

    def test_missing_build_ninja(self):
        (self.build / "build.ninja").unlink()
        self.assert_unsafe(F.freshness(str(self.root), str(self.build), self.arts), "unreliable")

    def test_missing_cmakecache(self):
        (self.build / "CMakeCache.txt").unlink()
        self.assert_unsafe(F.freshness(str(self.root), str(self.build), self.arts), "unreliable", "CMakeCache")

    def test_artifact_without_rule_is_ambiguous(self):
        touch(self.build / "Other.exe", BUILT)
        r = F.freshness(str(self.root), str(self.build), self.arts + [str(self.build / "Other.exe")])
        self.assert_unsafe(r, "Other.exe", "no build rule")
        self.assertTrue(r["targets"]["Motiva.exe"]["fresh"])  # the others are still judged on their own

    def test_missing_input_is_ambiguous(self):
        (self.root / "scripts" / "browser" / "host.cpp").unlink()
        r, t = self.run_()
        self.assert_unsafe(r, "MotivaBrowserHost.exe", "missing")
        self.assertEqual(t, {"Motiva.exe": True, "MotivaBrowserHost.exe": False})

    def test_no_project_source_is_ambiguous(self):
        self.write_ninja(NINJA.replace("{root}/scripts/browser/host.cpp", "{qt}/lib/libQt6Core.a"))
        r, t = self.run_()
        self.assert_unsafe(r, "no project source")
        self.assertFalse(t["MotivaBrowserHost.exe"])

    def test_unparseable_and_unsupported_ninja(self):
        self.write_ninja(NINJA + "build broken line without colon\n")
        self.assert_unsafe(F.freshness(str(self.root), str(self.build), self.arts), "unreliable", "unparseable")
        self.write_ninja(NINJA + "subninja other.ninja\n")
        self.assert_unsafe(F.freshness(str(self.root), str(self.build), self.arts), "subninja")
        self.write_ninja("# nothing here\n")
        self.assert_unsafe(F.freshness(str(self.root), str(self.build), self.arts), "no build edges")

    def test_missing_artifact(self):
        (self.build / "MotivaBrowserHost.exe").unlink()
        r, t = self.run_()
        self.assert_unsafe(r, "artifact missing")
        self.assertTrue(t["Motiva.exe"])

    def test_deploy_local_uses_this_implementation(self):
        self.assertIs(_deploy_qt.freshness, F.freshness)
        src = (SCRIPTS / "deploy_local.py").read_text(encoding="utf-8")
        self.assertIn("allow_stale", src)  # the explicit override is still the only way past a stale verdict


if __name__ == "__main__":
    unittest.main()
