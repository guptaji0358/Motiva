"""Runs one authorised step: bounded attempts, verification, diagnosis and recognised recoveries.

Runs in a worker thread and touches no shared state; the engine records what it returns.
"""
import os
import sys
import time

from . import policy
from .diagnose import diagnose
from .exec_actions import REGISTRY
from .util import clean_env, digest, file_hash, hash_patterns
from .verify import verify

# Re-running these cannot repeat a side effect outside the scratch root.
REPEATABLE = {"cmake_configure", "cmake_build", "qt_test", "command", "python_script", "lias_automation"}


def step_env(step, extra_sets):
    env = clean_env({"PYTHONDONTWRITEBYTECODE": "1", **step["env"].get("set", {})})
    key = next((k for k in env if k.upper() == "PATH"), "PATH")
    prepend = list(step["env"].get("prepend_path", []))
    for fix in extra_sets:
        if fix["type"] == "prepend_env":
            prepend = fix["values"] + prepend
        else:
            env[fix["var"]] = fix["value"]
    if prepend:
        env[key] = os.pathsep.join(prepend + [env.get(key, "")])
    return env


def evidence_key(step, root):
    """Identity of a cacheable check: definition + content of every declared input. None = cannot be reused."""
    inputs = hash_patterns(root, step["inputs"])
    if not inputs or any(v in ("missing", "no-match") for v in inputs.values()):
        return None
    exe = step["params"].get("exe")
    exe_hash = file_hash(exe) if exe else None
    if exe_hash == "missing":
        return None
    return digest({"action": step["action"], "params": step["params"], "env": step["env"], "expect": step["expect"],
                   "inputs": inputs, "exe": exe_hash, "python": sys.version, "path": os.environ.get("PATH", "")
                   if step["action"] != "qt_test" else None})


def run_step(step, ctx, permission, may_recover):
    """ctx: {paths, root, log_dir, results, run_no}. Returns a partial step record."""
    attempts, diagnoses, fixes, used_fix_types = [], [], [], set()
    retries_left = step["retries"]
    repeatable = step["action"] in REPEATABLE and permission != policy.APPROVAL
    number = 0
    while True:
        number += 1
        local = dict(ctx, env=step_env(step, fixes),
                     log_base=os.path.join(ctx["log_dir"], f"{step['id']}.r{ctx['run_no']}a{number}"))
        started = time.time()
        try:
            out = REGISTRY[step["action"]](step, local)
            checked = verify(step, out, ctx["root"])
        except Exception as e:  # a broken action is a failed step, never a silent success
            out = {"exit_code": None, "error": f"{type(e).__name__}: {e}", "data": {}, "log": None,
                   "commands": [], "output": ""}
            checked = {"status": "failed", "evidence": {"exit_code": None, "log": None},
                       "checks": [{"check": "action ran", "expected": "no internal error",
                                   "actual": out["error"], "ok": False}]}
        attempts.append({"n": number, "run": ctx.get("run_no"), "status": checked["status"], "exit_code": out["exit_code"],
                         "error": out["error"], "commands": out["commands"],
                         "duration_s": round(time.time() - started, 2)})
        if diagnoses and diagnoses[-1]["fix"]:
            diagnoses[-1]["fix"]["result"] = "verified" if checked["status"] == "verified" else "did not fix it"
        if checked["status"] != "failed":
            break
        found = diagnose(step, out)
        found["fix"] = None
        diagnoses.append(found)
        recovery = found["recovery"]
        if not recovery:
            found["stopped"] = "needs the task owner's judgment"
            break
        if not repeatable:
            found["stopped"] = "this step is never repeated automatically"
            break
        if not may_recover:
            found["stopped"] = "recovery available but the task has no 'recovery' grant"
            break
        if recovery["type"] == "retry":
            if retries_left <= 0:
                found["stopped"] = "retry budget exhausted" if step["retries"] else "no retries allowed for this step"
                break
            retries_left -= 1
            time.sleep(ctx.get("retry_delay", 1.0) * (step["retries"] - retries_left))
        else:  # each environment fix is tried once at most
            marker = digest(recovery)
            if marker in used_fix_types:
                found["stopped"] = "the same environment fix was already tried"
                break
            used_fix_types.add(marker)
            fixes.append(recovery)
        found["fix"] = {"applied": recovery, "result": None}
    return {"state": checked["status"], "attempts": attempts, "verification": checked, "data": out["data"],
            "diagnoses": diagnoses}
