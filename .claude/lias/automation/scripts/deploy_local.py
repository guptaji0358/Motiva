r"""Build, then deploy Motiva.exe, MotivaBrowserHost.exe and scripts/extension/ (no tests) to out/deployment/resources/bin.

Input JSON (all optional, real JSON types only): {"dry_run": bool, "qt_root": "C:\Qt", "skip_build": bool,
"allow_stale_artifacts": bool}. Qt root: input qt_root > env MOTIVA_QT_ROOT / QT_ROOT > installed C:\Qt. The Qt kit and MinGW toolchain are
taken from the existing out/build/CMakeCache.txt (never reconfigured) and must lie under the Qt root.
skip_build deploys existing artifacts only if EACH exe is newer than its own build inputs (per-target, from build.ninja, see
_deploy_fresh.py); otherwise it refuses unless
allow_stale_artifacts is true. Never kills processes: refuses if Motiva.exe / MotivaBrowserHost.exe run from out/.
Deployment is staged, backed up, applied, and rolled back on failure (best effort, NOT atomic; a kill/power loss during
apply can leave a mixed deployment, see _deploy_txn.py). Build timeout (540 s) stops cmake only, not its descendants.
The launcher (out/deployment/Motiva.exe), Qt DLLs and settings are never touched.
"""
import glob
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _common import qt_root as default_qt_root  # noqa: E402
from _deploy_qt import DeployError, freshness, run_capture, select_qt  # noqa: E402
from _deploy_txn import Deployment  # noqa: E402

ARGS = {"dry_run": bool, "skip_build": bool, "allow_stale_artifacts": bool, "qt_root": str}
STATE = {"dep": None, "stage": "validation"}


def out(obj):
    print(json.dumps(obj))
    sys.exit(0)


def fail(msg, **kw):
    dep = STATE["dep"]
    res = {"ok": False, "error": msg, "operation": STATE["stage"],
           "destination_changed": bool(dep and dep.changed)}
    if dep and dep.changed:
        res["rollback"] = "succeeded" if dep.rolled_back else "FAILED"
    res.update(kw)
    out(res)


def parse_opts(text):
    try:
        opts = json.loads(text or "{}")
    except ValueError as ex:
        fail("input is not valid JSON: %s" % ex)
    if not isinstance(opts, dict):
        fail("input must be a JSON object")
    unknown = sorted(set(opts) - set(ARGS))
    if unknown:
        fail("unknown input keys: %s" % unknown)
    for k, v in opts.items():
        # bool is a subclass of int, but "false"/0/1 are not Booleans: accept only real JSON true/false.
        if ARGS[k] is bool and not isinstance(v, bool):
            fail("input %s must be a JSON boolean (true/false), got %s" % (k, type(v).__name__))
        if ARGS[k] is str and (not isinstance(v, str) or "\x00" in v):
            fail("input %s must be a string" % k)
    return opts


def running_from(paths):
    """Motiva processes whose executable lies under any of paths. Unknown path => treated as blocking."""
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='Motiva.exe' OR Name='MotivaBrowserHost.exe'\" | "
          "Select-Object ProcessId,Name,ExecutablePath | ConvertTo-Json -Compress")
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        fail("process check timed out after 30s, refusing to deploy", command="powershell Get-CimInstance")
    if r.returncode:
        fail("process check failed, refusing to deploy: %s" % r.stderr.strip()[:200])
    txt = r.stdout.strip()
    try:
        procs = json.loads(txt) if txt else []
    except ValueError as ex:
        fail("process check returned unparseable output, refusing to deploy: %s" % ex)
    if isinstance(procs, dict):
        procs = [procs]
    norm = [os.path.normcase(os.path.abspath(p)) + os.sep for p in paths]
    hits = []
    for p in procs:
        exe = p.get("ExecutablePath")
        if not exe or any(os.path.normcase(os.path.abspath(exe)).startswith(n) for n in norm):
            hits.append({"pid": p.get("ProcessId"), "name": p.get("Name"), "path": exe or "unknown"})
    return hits


def extension_files(src):
    """(abs source, relative path) for every file, skipping anything named 'tests' (as before)."""
    res = []
    for d, dirs, files in os.walk(src):
        dirs[:] = [x for x in dirs if x != "tests"]
        for f in files:
            if f != "tests":
                p = os.path.join(d, f)
                res.append((p, os.path.relpath(p, src)))
    return res


def main():
    opts = parse_opts(sys.stdin.read())
    dry = opts.get("dry_run", False)
    skip_build = opts.get("skip_build", False)
    allow_stale = opts.get("allow_stale_artifacts", False)
    root = os.environ.get("LIAS_PROJECT_ROOT")
    if not root or not os.path.isdir(root):
        fail("LIAS_PROJECT_ROOT is not set to an existing directory")
    qt_root = opts.get("qt_root") or default_qt_root()
    build = os.path.join(root, "out", "build")
    bindir = os.path.join(root, "out", "deployment", "resources", "bin")
    ext_src = os.path.join(root, "scripts", "extension")
    ext_dst = os.path.join(bindir, "extension")
    exes = ["Motiva.exe", "MotivaBrowserHost.exe"]
    artifacts = [os.path.join(build, e) for e in exes]

    # ---- 1. validate everything (nothing is changed) ----
    try:
        qt_dirs, qt_info = select_qt(qt_root, build, root)
    except DeployError as ex:
        fail(ex.msg, **ex.kw)
    except OSError as ex:
        fail("cannot read Qt/CMake configuration: %s" % ex, path=getattr(ex, "filename", None))
    if not os.path.isdir(bindir):
        fail("deployment bin directory missing: %s" % bindir)
    if not os.path.isfile(os.path.join(ext_src, "manifest.json")):
        fail("extension source invalid (no manifest.json): %s" % ext_src)
    leftovers = glob.glob(os.path.join(bindir, ".deploy-stage-*"))
    if leftovers:
        fail("leftover staging directory from an interrupted deployment (may hold backups); inspect, then remove "
             "manually", paths=leftovers)
    hits = running_from([os.path.join(root, "out")])
    if hits:
        fail("Motiva processes are running from out/; close them manually (nothing was killed)", running=hits)

    fresh = None
    if skip_build:
        fresh = freshness(root, build, artifacts)
        if not fresh["fresh"] and not allow_stale and not dry:
            fail("skip_build refused: existing artifacts cannot be shown to be up to date (%s). Run without "
                 "skip_build, or pass allow_stale_artifacts=true to deploy them anyway." % fresh["reason"],
                 freshness=fresh)

    env = dict(os.environ)
    # Qt's bundled CMake is a fallback after the parent PATH. The program is resolved against this PATH here because
    # Windows would otherwise look "cmake" up on the PARENT's PATH only.
    bundled = os.path.join(qt_root, "Tools", "CMake_64", "bin")
    env["PATH"] = os.pathsep.join(qt_dirs + [env.get("PATH", ""), bundled])
    cmake = shutil.which("cmake", path=env["PATH"]) or "cmake"
    plan = {"qt": qt_info, "copy": [os.path.join(bindir, e) for e in exes], "extension": ext_dst,
            "build": not skip_build, "rollback": "best-effort, not atomic"}
    if fresh is not None:
        plan["skip_build_freshness"] = fresh
        plan["would_refuse"] = (not fresh["fresh"]) and not allow_stale
    if dry:  # report only: no build, no staging, no copy below this point is reachable
        missing = [e for e in exes if not os.path.isfile(os.path.join(build, e))]
        out({"ok": True, "dry_run": True, "plan": plan, "build_artifacts_missing_now": missing})

    # ---- 2. build ----
    if not skip_build:
        STATE["stage"] = "build (cmake --build out/build)"
        try:
            code, text = run_capture([cmake, "--build", "out/build"], root, 540, env)
        except subprocess.TimeoutExpired:
            fail("build timed out after 540s; only the cmake process was stopped (Ninja/compiler child processes may still be "
                 "running and writing to out/build; none were killed); no deployment destination was changed",
                 command="cmake --build out/build")
        except OSError as ex:
            fail("could not start cmake: %s" % ex)
        if code:
            errs = [x.strip()[:300] for x in text.splitlines() if "error" in x.lower() or "FAILED" in x][:40]
            fail("build failed", exit_code=code, errors=errs)
        STATE["stage"] = "validation"
    for a in artifacts:
        if not os.path.isfile(a):
            fail("build artifact missing: %s" % a)
    if skip_build and allow_stale and not fresh["fresh"]:
        plan["warning"] = "deploying artifacts not proven fresh (allow_stale_artifacts): " + fresh["reason"]
    hits = running_from([os.path.join(root, "out")])  # re-check after the (long) build
    if hits:
        fail("Motiva processes started during build; aborting before copy", running=hits)

    # ---- 3. stage + back up (destinations untouched), 4. apply, 5. rollback on failure ----
    dep = Deployment(bindir)
    STATE["dep"] = dep
    try:
        STATE["stage"] = "staging"
        for e in exes:
            dep.add(os.path.join(build, e), os.path.join(bindir, e))
        for src, rel in extension_files(ext_src):
            dep.add(src, os.path.join(ext_dst, rel))
        dep.stage_all()
        STATE["stage"] = "applying files to out/deployment/resources/bin"
        dep.apply()
    except (Exception, KeyboardInterrupt) as ex:
        failed_at = STATE["stage"]
        path = getattr(ex, "filename", None)
        if dep.changed:
            dep.rollback()
        kept = dep.cleanup()
        extra = {"path": path, "rollback_errors": dep.rollback_errors} if dep.rollback_errors else {"path": path}
        if kept:
            extra["backups_kept_at"] = kept
        fail("deployment failed during %s: %s" % (failed_at, ex), **extra)
    dep.cleanup()
    out({"ok": True, "deployed": {e: os.path.getsize(os.path.join(bindir, e)) for e in exes},
         "extension": ext_dst, "extension_files": len(dep.items) - len(exes),
         "files_newly_created": sum(1 for i in dep.items if not i["existed"]), "warning": plan.get("warning")})


try:
    main()
except SystemExit:
    raise
except Exception as ex:  # last resort: structured error, no environment dump
    fail("unexpected %s during %s: %s" % (type(ex).__name__, STATE["stage"], ex))
