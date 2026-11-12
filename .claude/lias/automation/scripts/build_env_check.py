"""Check the Motiva build toolchain (existence only; read-only). stdout: one JSON line."""
import glob
import json
import os
import re
import shutil
import sys

json.loads(sys.stdin.read() or "{}")
qt_root = r"D:\Apps\Qt6"
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
    "cmake": shutil.which("cmake") is not None,
    "ninja": shutil.which("ninja") is not None,
    "build_dir": os.path.isdir(os.path.join(root, "build")),
}
cache_prefix = None
cache = os.path.join(root, "build", "CMakeCache.txt")
if os.path.isfile(cache):
    with open(cache, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("CMAKE_PREFIX_PATH"):
                cache_prefix = line.split("=", 1)[1].strip()
                break
warnings = []
if cache_prefix and qt and os.path.normcase(os.path.normpath(cache_prefix)) != os.path.normcase(os.path.normpath(qt)):
    warnings.append("build/CMakeCache.txt CMAKE_PREFIX_PATH differs from newest installed Qt kit")
print(json.dumps({"ok": all(v for k, v in checks.items() if k != "build_dir"), "checks": checks,
                  "qt_kit": qt, "mingw_bin": mingw[-1] if mingw else "", "cmake_prefix_in_cache": cache_prefix,
                  "notes": warnings}))
