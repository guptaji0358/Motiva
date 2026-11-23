"""Qt kit selection/cache validation, the build runner; re-exports per-target freshness (helper for deploy_local.py)."""
import os
import re
import subprocess
import tempfile

from _deploy_fresh import freshness  # per-target freshness lives in _deploy_fresh.py

__all__ = ["DeployError", "freshness", "run_capture", "select_qt"]


class DeployError(Exception):
    def __init__(self, msg, **kw):
        Exception.__init__(self, msg)
        self.msg, self.kw = msg, kw


def run_capture(cmd, cwd, timeout, env=None):
    """(exit code, combined output). Output goes to a temp FILE, not pipes: subprocess.run() re-reads the pipes after
    a timeout and, on Windows, blocks until every descendant (Ninja, compilers) releases them, so its timeout did not
    return in time. Only the direct child is killed on timeout/interrupt; its descendants are NOT (no process-tree
    kill is attempted, see lias.md), they may keep running and finish or fail on their own."""
    with tempfile.TemporaryFile() as sink:
        p = subprocess.Popen(cmd, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=sink, stderr=subprocess.STDOUT)
        try:
            code = p.wait(timeout=timeout)
        except BaseException:  # TimeoutExpired, KeyboardInterrupt: stop our own direct child only
            p.kill()
            p.wait()
            raise
        sink.seek(0)
        return code, sink.read().decode("utf-8", errors="replace")


def norm(p):
    return os.path.normcase(os.path.normpath(p))


def version_key(name):
    """'6.12.0' -> (6, 12, 0); non-numeric names -> None (ignored). Numeric, so 6.12 > 6.9."""
    if re.fullmatch(r"\d+(\.\d+)*", name):
        return tuple(int(x) for x in name.split("."))
    return None


def toolchain_key(name):
    m = re.fullmatch(r"mingw(\d+)_64", name)
    return int(m.group(1)) if m else None


def list_kits(qt_root):
    """[(version_tuple, kit_dir)] for numeric <root>/<ver>/mingw_64, newest first."""
    kits = []
    for n in os.listdir(qt_root):
        k = version_key(n)
        d = os.path.join(qt_root, n, "mingw_64")
        if k and os.path.isdir(d):
            kits.append((k, d))
    return sorted(kits, reverse=True)


def read_cache(path):
    vals = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            m = re.match(r"^(Qt6_DIR|CMAKE_CXX_COMPILER|CMAKE_HOME_DIRECTORY|CMAKE_GENERATOR)(?::[A-Z]+)?=(.*)$", ln.strip())
            if m:
                vals[m.group(1)] = m.group(2)
    return vals


def select_qt(qt_root, build_dir, root):
    """Return (path dirs, info). The existing CMakeCache is authoritative: it is never reconfigured, so the
    toolchain must match it. Fails with an explanation on any mismatch; never picks a different kit silently."""
    if not os.path.isdir(qt_root):
        raise DeployError("Qt root not found: %s (set qt_root or MOTIVA_QT_ROOT)" % qt_root)
    cache_file = os.path.join(build_dir, "CMakeCache.txt")
    if not os.path.isfile(cache_file):
        raise DeployError("out/build is not configured (no CMakeCache.txt); not reconfiguring automatically")
    c = read_cache(cache_file)
    for k in ("Qt6_DIR", "CMAKE_CXX_COMPILER"):
        if not c.get(k) or c[k].endswith("-NOTFOUND"):
            raise DeployError("CMakeCache.txt has no usable %s" % k)
    if c.get("CMAKE_HOME_DIRECTORY") and norm(c["CMAKE_HOME_DIRECTORY"]) != norm(root):
        raise DeployError("build cache belongs to another source tree", cache_source=c["CMAKE_HOME_DIRECTORY"],
                          project=root)
    # Qt6_DIR = <kit>/lib/cmake/Qt6
    kit = os.path.normpath(os.path.join(c["Qt6_DIR"], "..", "..", ".."))
    compiler = os.path.normpath(c["CMAKE_CXX_COMPILER"])
    tool_dir = os.path.dirname(os.path.dirname(compiler))  # <Tools>/mingwNNNN_64
    problems = []
    root_n = norm(qt_root) + os.sep
    if not (norm(kit) + os.sep).startswith(root_n):
        problems.append("cached Qt6_DIR kit %s is outside requested Qt root %s" % (kit, qt_root))
    if not (norm(compiler)).startswith(root_n):
        problems.append("cached C++ compiler %s is outside requested Qt root %s" % (compiler, qt_root))
    if os.path.basename(kit).lower() != "mingw_64" or version_key(os.path.basename(os.path.dirname(kit))) is None:
        problems.append("cached Qt kit %s is not a numeric-version mingw_64 kit" % kit)
    if toolchain_key(os.path.basename(tool_dir).lower()) is None:
        problems.append("cached compiler %s is not a Qt-bundled mingwNNNN_64 toolchain" % compiler)
    if problems:
        raise DeployError("Qt kit/toolchain conflicts with existing out/build CMakeCache.txt; refusing to build "
                          "(not reconfiguring or overwriting the cache). Fix qt_root or reconfigure manually.",
                          mismatches=problems, cache_qt6_dir=c["Qt6_DIR"], cache_compiler=c["CMAKE_CXX_COMPILER"],
                          requested_qt_root=qt_root)
    dirs = [os.path.join(tool_dir, "bin"), os.path.join(kit, "bin")]
    for d in dirs:
        if not os.path.isdir(d):
            raise DeployError("missing Qt directory: %s" % d)
    if not os.path.isfile(compiler):
        raise DeployError("cached compiler not found on disk: %s" % compiler)
    newest = list_kits(qt_root)
    info = {"kit": kit, "toolchain": tool_dir, "compiler": compiler, "generator": c.get("CMAKE_GENERATOR")}
    if newest and norm(newest[0][1]) != norm(kit):
        info["note"] = ("newer Qt kit installed (%s) but the build cache is configured for %s; using the cache's kit"
                        % (newest[0][1], kit))
    return dirs, info

