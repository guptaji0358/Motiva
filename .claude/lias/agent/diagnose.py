"""Diagnosis: match known failure patterns and name the one safe recovery, if there is one.

Deliberately a table of facts, not reasoning. Anything unrecognised is escalated, never guessed at.
"""
import os
import re

from .util import digest

MISSING_DLL = {0xC0000135, -1073741515, 0xC000007B, -1073741701}
LOCKED = re.compile(r"(?i)being used by another process|cannot open output file .*Permission denied|"
                    r"unable to rename temporary|The process cannot access the file|ninja: error: .*Permission denied")
TOOL_CRASH = re.compile(r"(?i)(cc1(plus)?(\.exe)?: .*(failed|cannot execute)|windres(\.exe)?: .*(failed|can't)|"
                        r"CreateProcess: No such file or directory)")
PORT_BUSY = re.compile(r"(?i)address already in use|only one usage of each socket address")
NO_MODULE = re.compile(r"ModuleNotFoundError: No module named '([\w.]+)'")
CACHE_MISMATCH = re.compile(r"(?i)does not match the source .*used to generate cache|"
                            r"current CMakeCache\.txt directory .* is different")
QPA = re.compile(r"(?i)(no Qt platform plugin could be initialized|could not find the Qt platform plugin)")
COMPILE = re.compile(r"^(.{0,260}?:\d+(?::\d+)?: (?:fatal )?error: .+)$", re.M)
LINK = re.compile(r"^(.*(?:undefined reference to .+|ld returned \d+ exit status))$", re.M)
CMAKE_ERR = re.compile(r"^(CMake Error.*(?:\n  .+){0,4})", re.M)


def diagnose(step, out):
    """Returns {pattern, kind, summary, details, recovery, signature}. recovery is None => escalate."""
    text, data, code = out["output"] or "", out["data"], out["exit_code"]
    err = out["error"] or ""

    def d(pattern, kind, summary, recovery=None, details=()):
        details = [str(x)[:300] for x in details][:25]
        return {"pattern": pattern, "kind": kind, "summary": summary, "details": details, "recovery": recovery,
                "signature": digest([step["id"], pattern, details])[:16]}

    path_dirs = [x for x in step["params"].get("recovery_path", []) if os.path.isdir(x)]
    prepend = {"type": "prepend_env", "var": "PATH", "values": path_dirs} if path_dirs else None
    if data.get("timed_out"):
        return d("timeout", "transient", err, {"type": "retry"})
    if err.startswith("LIAS database needs"):
        return d("lias-database-mismatch", "needs_decision", err)
    if err.startswith("program not found"):
        return d("program-not-found", "environment", err, prepend)
    if code in MISSING_DLL:
        return d("missing-dll", "environment", f"process could not load a required DLL (exit {code})", prepend)
    plugins = step["params"].get("qt_plugins")
    if QPA.search(text):
        fix = {"type": "set_env", "var": "QT_QPA_PLATFORM_PLUGIN_PATH", "value": os.path.join(plugins, "platforms")} \
            if plugins and os.path.isdir(plugins) else None
        return d("qt-platform-plugin", "environment", "Qt platform plugin was not found", fix)
    m = NO_MODULE.search(text)
    if m:
        return d("python-module-missing", "needs_decision",
                 f"Python module '{m.group(1)}' is not installed; installing packages needs approval")
    if CACHE_MISMATCH.search(text):
        return d("cmake-cache-mismatch", "needs_decision",
                 "build directory belongs to another source tree; choose a new scratch root (nothing was deleted)")
    for pat, name, summary in ((LOCKED, "file-locked", "a file was locked (sync client / antivirus)"),
                               (TOOL_CRASH, "toolchain-spawn", "toolchain helper failed to run (known transient)"),
                               (PORT_BUSY, "port-busy", "test port was already in use")):
        hit = pat.search(text)
        if hit:
            return d(name, "transient", summary, {"type": "retry"}, [hit.group(0)])
    totals = data.get("totals")
    if step["action"] == "qt_test" and totals and totals["failed"]:
        return d("test-failures", "needs_decision", f"{totals['failed']} test(s) failed",
                 details=data.get("failures", []) + data.get("failure_locations", []))
    errors = list(dict.fromkeys(COMPILE.findall(text) + LINK.findall(text)))
    if errors:
        return d("compile-or-link-error", "needs_decision", f"{len(errors)} compiler/linker error line(s)",
                 details=errors)
    cm = CMAKE_ERR.findall(text)
    if cm:
        return d("cmake-error", "needs_decision", "CMake configuration failed", details=cm)
    tail = [x for x in text.strip().splitlines()[-8:] if x.strip()]
    return d("unrecognised", "unknown", err or f"exit {code}; no known pattern matched", details=tail)
