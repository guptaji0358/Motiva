"""Verification engine: a step is verified only when its actual outcome matches an expectation."""
import os

from .inspect_actions import OWN_PATHS  # noqa: F401  (re-exported for tests)
from .util import file_hash

EXPECT_KEYS = {"exit_code", "files", "output_contains", "output_not_contains", "min_passed", "max_skipped",
               "unchanged", "exit_code_is_sufficient"}


def verify(step, out, root):
    """Returns {status: verified|failed|unverified, checks: [{check, expected, actual, ok}], evidence: {}}."""
    exp, action, data = step["expect"], step["action"], out["data"]
    checks, evidence = [], {"exit_code": out["exit_code"], "log": out["log"]}
    positive = False  # at least one check that proves more than "it ran"

    def chk(name, expected, actual, ok):
        checks.append({"check": name, "expected": expected, "actual": actual, "ok": bool(ok)})
        return ok

    unknown = set(exp) - EXPECT_KEYS
    if unknown:
        chk("expectation keys", "known keys", f"unknown: {sorted(unknown)}", False)
    want_code = exp.get("exit_code", 0)
    chk("exit code", want_code, out["exit_code"], out["exit_code"] == want_code)
    if out["error"] and out["exit_code"] in (None, 0):
        chk("no execution error", "none", out["error"], False)

    for rel in exp.get("files", []):
        path = os.path.join(root, rel)
        ok = os.path.isfile(path) and os.path.getsize(path) > 0
        chk("artifact exists", rel, "present" if ok else "missing or empty", ok)
        if ok:
            evidence.setdefault("files", {})[rel] = file_hash(path)
            positive = True
    for key, should in (("output_contains", True), ("output_not_contains", False)):
        for text in exp.get(key, []):
            found = text in out["output"]
            chk(key.replace("_", " "), text, "found" if found else "absent", found == should)
            positive = positive or should

    if action == "qt_test":
        t = data.get("totals")
        if chk("QtTest totals reported", "a Totals line", t or "none", t is not None):
            chk("failed tests", 0, t["failed"], t["failed"] == 0)
            least = exp.get("min_passed", 1)
            chk("passed tests", f">= {least}", t["passed"], t["passed"] >= least)
            if "max_skipped" in exp:
                chk("skipped tests", f"<= {exp['max_skipped']}", t["skipped"], t["skipped"] <= exp["max_skipped"])
            evidence["totals"], positive = t, True
        evidence["exe_sha256"] = data.get("exe_sha256")
    elif action == "cmake_configure":
        for name in ("CMakeCache.txt", "build.ninja"):
            if name == "build.ninja" and step["params"].get("generator", "Ninja") != "Ninja":
                continue
            ok = os.path.isfile(os.path.join(step["params"]["build_dir"], name))
            chk("configure output", name, "present" if ok else "missing", ok)
        positive = True
    elif action in ("git_compare", "registry_snapshot"):
        if "unchanged" in exp:
            chk("state unchanged", exp["unchanged"], data.get("unchanged"), data.get("unchanged") == exp["unchanged"])
        positive = True
    elif action == "edit_file":
        path = os.path.join(root, step["params"]["path"])
        ok = bool(data.get("sha256")) and file_hash(path) == data.get("sha256")
        chk("file content on disk", data.get("sha256"), file_hash(path), ok)
        evidence["sha256"], positive = data.get("sha256"), True
    elif action == "lias_automation":
        result = (data.get("envelope") or {}).get("result") or {}
        positive = positive or result.get("ok") is True
    elif action in ("git_snapshot", "source_snapshot", "process_check", "file_check", "tool_check",
                    "cmake_cache_inspect"):
        positive = True  # the observation itself is the result
        evidence["observation"] = data.get("fingerprint")

    if any(not c["ok"] for c in checks):
        status = "failed"
    elif positive or exp.get("exit_code_is_sufficient") is True:
        status = "verified"
    else:
        status = "unverified"
        checks.append({"check": "outcome proof", "expected": "an expectation beyond the exit code",
                       "actual": "none defined or none observable", "ok": False})
    return {"status": status, "checks": checks, "evidence": evidence}
