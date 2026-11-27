"""Safe `cmake --build` shared by build_compile.py and precommit_check.py (not an automation).

Never kills processes: refuses if any process runs from the directory that the build would overwrite.
Never configures: the build directory must already hold a CMakeCache.txt for this source tree.
"""
import json
import os
import shutil
import subprocess

from _deploy_qt import run_capture

DEFAULT_BUILD_DIR = os.path.join("out", "build")


def _norm(p):
    return os.path.normcase(os.path.realpath(p))


def list_processes():
    """[{ProcessId, Name, ExecutablePath}] for every process; raises RuntimeError if it cannot be established."""
    ps = ("Get-CimInstance Win32_Process | Select-Object ProcessId,Name,ExecutablePath | "
          "ConvertTo-Json -Compress")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", timeout=30)
        if r.returncode:
            raise RuntimeError(r.stderr.strip()[:200] or "exit %d" % r.returncode)
        procs = json.loads(r.stdout)
    except (OSError, subprocess.TimeoutExpired, ValueError) as ex:
        raise RuntimeError(str(ex))
    return [procs] if isinstance(procs, dict) else procs


def running_from(directory, procs):
    """Processes whose executable lies under directory. A Motiva* process with an unreadable path also blocks."""
    prefix = os.path.join(_norm(directory), "")
    hits = []
    for p in procs:
        exe, name = p.get("ExecutablePath"), p.get("Name") or ""
        if (exe and _norm(exe).startswith(prefix)) or (not exe and name.lower().startswith("motiva")):
            hits.append({"pid": p.get("ProcessId"), "name": name, "path": exe or "unknown"})
    return hits


def _cache_source(cache_file):
    with open(cache_file, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line.startswith("CMAKE_HOME_DIRECTORY"):
                return line.split("=", 1)[1].strip()
    return None


def safe_build(root, build_dir, env, timeout=540):
    """-> {"build_dir", "exit_code", "output"} when cmake ran, else {"build_dir", "refused": reason, ...}."""
    bdir = os.path.normpath(os.path.join(root, build_dir or DEFAULT_BUILD_DIR))
    res = {"build_dir": bdir}
    cache = os.path.join(bdir, "CMakeCache.txt")
    if not os.path.isfile(cache):
        return dict(res, refused="build directory is not configured (no CMakeCache.txt); configure it with cmake first")
    source = _cache_source(cache)
    if not source or _norm(source) != _norm(root):
        return dict(res, refused="build directory was configured for another source tree: %s" % source)
    try:
        hits = running_from(bdir, list_processes())
    except RuntimeError as ex:
        return dict(res, refused="process check failed, not building: %s" % ex)
    if hits:
        return dict(res, running=hits, refused="processes are running from the build directory; close them manually "
                                               "and retry (nothing was killed)")
    cmake = shutil.which("cmake", path=env.get("PATH"))
    if not cmake:
        return dict(res, refused="cmake not found; check the Qt root (MOTIVA_QT_ROOT, default C:\\Qt)")
    try:
        code, text = run_capture([cmake, "--build", bdir], root, timeout, env)
    except subprocess.TimeoutExpired:
        return dict(res, refused="build timed out after %ds; only cmake was stopped, its Ninja/compiler children "
                                 "may still be running" % timeout)
    return dict(res, exit_code=code, output=text)
