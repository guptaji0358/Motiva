"""Shared helpers for LIAS git/build automations (not an automation itself)."""
import glob
import os
import re
import shutil
import subprocess

TRAILER = "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
PR_FOOTER = "Generated with [Claude Code](https://claude.com/claude-code)"
SECRET_HINTS = (".env", "id_rsa", ".pem", ".pfx", "credentials", "secret", "token")


def tool(name, extra=()):
    p = shutil.which(name)
    if p:
        return p
    for d in extra:
        c = os.path.join(d, name + ".exe")
        if os.path.isfile(c):
            return c
    return None


QT_ROOTS = (r"C:\Qt", r"D:\Apps\Qt6")


def qt_root():
    """Folder holding Tools\\ and <version>\\. env MOTIVA_QT_ROOT / QT_ROOT wins, else the first known install."""
    override = os.environ.get("MOTIVA_QT_ROOT") or os.environ.get("QT_ROOT")
    if override:
        return override
    for r in QT_ROOTS:
        if os.path.isdir(os.path.join(r, "Tools")):
            return r
    return QT_ROOTS[0]


def qt_dirs(root=None):
    """Existing PATH dirs for the Qt build: newest MinGW, newest mingw_64 kit, bundled CMake and Ninja."""
    root = root or qt_root()

    def newest(pattern):
        found = glob.glob(os.path.join(glob.escape(root), *pattern))
        return sorted(found, key=lambda p: [int(x) for x in re.findall(r"\d+", p[len(root):])])[-1:]

    dirs = newest(("Tools", "mingw*_64", "bin")) + newest(("[0-9]*", "mingw_64", "bin"))
    dirs += newest(("Tools", "CMake_64", "bin")) + newest(("Tools", "Ninja"))
    return dirs


def qt_env():
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join(qt_dirs() + [env.get("PATH", "")])
    return env


def run(cmd, cwd, timeout=120, env=None):
    if env is not None:  # Windows looks the program up on the PARENT's PATH, so resolve it against env's
        cmd = [shutil.which(cmd[0], path=env.get("PATH")) or cmd[0]] + list(cmd[1:])
    r =subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout, env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")
