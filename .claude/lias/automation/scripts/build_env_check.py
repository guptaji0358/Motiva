"""Check the Motiva build toolchain (existence only; read-only). stdout: one JSON line."""
import glob
import json
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import qt_env, qt_root as find_qt_root  # noqa: E402

json.loads(sys.stdin.read() or "{}")
qt_root = find_qt_root()
path = qt_env()["PATH"]  # the PATH the build automations use
root = os.environ["LIAS_PROJECT_ROOT"]


def ver_key(p):
    return tuple(int(x) for x in re.findall(r"\d+", os.path.basename(os.path.dirname(p))))


kits = sorted(glob.glob(os.path.join(qt_root, "*", "mingw_64")), key=ver_key)
qt = kits[-1] if kits else ""
mingw = sorted(glob.glob(os.path.join(qt_root, "Tools", "mingw*_64", "bin")))
checks = {
    "qt_mingw_kit": bool(qt),
    "qt_cmake_config": bool(qt) and os.path.isfile(os.path.join(qt, "lib", "cmake", "Qt6", "Qt6Config.cmake")),
    "mingw_gpp": any(os.path.isfile(os.path.join(m, "g++.exe")) for m in mingw),
    "cmake": shutil.which("cmake", path=path) is not None,
    "ninja": shutil.which("ninja", path=path) is not None,
    "build_dir": os.path.isdir(os.path.join(root, "out", "build")),
}
cache_prefix = cache_source = None
cache = os.path.join(root, "out", "build", "CMakeCache.txt")
if os.path.isfile(cache):
    with open(cache, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("CMAKE_PREFIX_PATH"):
                cache_prefix = line.split("=", 1)[1].strip()
            elif line.startswith("CMAKE_HOME_DIRECTORY"):
                cache_source = line.split("=", 1)[1].strip()
warnings = []
if cache_source and os.path.normcase(os.path.normpath(cache_source)) != os.path.normcase(os.path.normpath(root)):
    warnings.append("out/build was configured for another source folder (%s); it must be reconfigured" % cache_source)
if cache_prefix and qt and os.path.normcase(os.path.normpath(cache_prefix)) != os.path.normcase(os.path.normpath(qt)):
    warnings.append("out/build/CMakeCache.txt CMAKE_PREFIX_PATH differs from newest installed Qt kit")
print(json.dumps({"ok": all(v for k, v in checks.items() if k != "build_dir"), "checks": checks,
                  "qt_kit": qt, "mingw_bin": mingw[-1] if mingw else "", "cmake_prefix_in_cache": cache_prefix,
                  "notes": warnings}))
