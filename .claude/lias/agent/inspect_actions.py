"""Read-only inspection actions. Each returns {exit_code, error, data, log, commands, output}."""
import csv
import io
import json
import os

from .process import run_process
from .util import clean_env, file_hash, hash_patterns, digest

# The agent's own bookkeeping must not look like a change made by the task.
OWN_PATHS = (".claude/lias/agent-data", ".claude/lias/database")


def outcome(exit_code=0, data=None, error=None, log=None, commands=(), output=""):
    return {"exit_code": exit_code, "error": error, "data": data or {}, "log": log,
            "commands": list(commands), "output": output}


def _git(ctx, step, args, name):
    argv = ["git", "--no-pager", "-c", "core.quotepath=false"] + args
    r = run_process(argv, ctx["root"], step["timeout"], clean_env({"GIT_OPTIONAL_LOCKS": "0"}),
                    ctx["log_base"] + f".{name}.log")
    return r, " ".join(["git"] + args)


def git_snapshot(step, ctx):
    commands, got = [], {}
    for name, args in (("head", ["rev-parse", "HEAD"]), ("branch", ["rev-parse", "--abbrev-ref", "HEAD"]),
                       ("status", ["status", "--porcelain", "--untracked-files=all"]),
                       ("staged", ["diff", "--cached", "--no-ext-diff", "--name-status"])):
        r, cmd = _git(ctx, step, args, name)
        commands.append({"cmd": cmd, "exit_code": r["exit_code"], "log": r["log"]})
        if r["exit_code"] != 0:
            return outcome(r["exit_code"], error=r["error"] or r["output"][-300:], log=r["log"], commands=commands)
        got[name] = r["output"].rstrip("\n")
    files = {}
    for line in got["status"].splitlines():
        code, rel = line[:2], line[3:].split(" -> ")[-1].strip().strip('"')
        if rel.startswith(OWN_PATHS):
            continue
        files[rel] = f"{code}|{file_hash(os.path.join(ctx['root'], rel))}"
    data = {"head": got["head"], "branch": got["branch"], "staged": got["staged"], "changed": files}
    data["fingerprint"] = digest(data)
    return outcome(0, data, commands=commands)


def git_compare(step, ctx):
    ref = step["params"].get("baseline")
    before = ctx["results"].get(ref, {}).get("data")
    if not before or "changed" not in before:
        return outcome(1, error=f"no baseline snapshot from step {ref!r}")
    now = git_snapshot(step, ctx)
    if now["exit_code"] != 0:
        return now
    after = now["data"]
    a, b = before["changed"], after["changed"]
    diff = {"added": sorted(set(b) - set(a)), "removed": sorted(set(a) - set(b)),
            "modified": sorted(k for k in set(a) & set(b) if a[k] != b[k]),
            "head_moved": before["head"] != after["head"], "branch_changed": before["branch"] != after["branch"],
            "index_changed": before["staged"] != after["staged"]}
    diff["unchanged"] = not any(diff.values())
    return outcome(0, diff, commands=now["commands"])


def source_snapshot(step, ctx):
    files = hash_patterns(ctx["root"], step["params"].get("patterns", []))
    missing = sorted(k for k, v in files.items() if v in ("missing", "no-match"))
    return outcome(0 if files and not missing else 1,
                   {"files": len(files), "fingerprint": digest(files), "unmatched": missing},
                   error=f"unmatched patterns: {missing}" if missing else None)


def process_check(step, ctx):
    names = [n.lower() for n in step["params"].get("names", [])]
    argv = ["tasklist", "/FO", "CSV", "/NH"] if os.name == "nt" else ["ps", "-eo", "comm=,pid="]
    r = run_process(argv, ctx["root"], step["timeout"], clean_env(), ctx["log_base"] + ".processes.log")
    cmd = [{"cmd": " ".join(argv), "exit_code": r["exit_code"], "log": r["log"]}]
    if r["exit_code"] != 0:
        return outcome(r["exit_code"], error=r["error"] or "process listing failed", commands=cmd)
    if os.name == "nt":
        rows = [(x[0], x[1]) for x in csv.reader(io.StringIO(r["output"])) if len(x) > 1]
    else:
        rows = [tuple(x.rsplit(None, 1)) for x in r["output"].splitlines() if len(x.split()) > 1]
    found = [{"name": n, "pid": int(p)} for n, p in rows if n.lower() in names and p.isdigit()]
    # The log holds every process on the machine; keep only what was asked about.
    with open(r["log"], "w", encoding="utf-8") as f:
        f.write(json.dumps(found))
    return outcome(0, {"checked": names, "running": found, "terminated": []}, log=r["log"], commands=cmd)


def file_check(step, ctx):
    paths = step["params"].get("paths", [])
    state = {p: os.path.exists(os.path.join(ctx["root"], p)) for p in paths}
    missing = [p for p, ok in state.items() if not ok]
    return outcome(1 if missing or not paths else 0, {"exists": state},
                   error=f"missing: {missing}" if missing else None)


def tool_check(step, ctx):
    p = step["params"]
    env = ctx["env"]
    found, commands, bad = {}, [], []
    for name, path in p.get("tools", {}).items():
        r = run_process([path, "--version"], ctx["root"], step["timeout"], env, ctx["log_base"] + f".{name}.log")
        commands.append({"cmd": f"{path} --version", "exit_code": r["exit_code"], "log": r["log"]})
        found[name] = (r["output"].splitlines() or [""])[0] if r["exit_code"] == 0 else None
        if r["exit_code"] != 0:
            bad.append(name)
    dirs = {d: os.path.isdir(d) for d in p.get("dirs", [])}
    bad += [d for d, ok in dirs.items() if not ok]
    return outcome(1 if bad else 0, {"tools": found, "dirs": dirs},
                   error=f"unavailable: {bad}" if bad else None, commands=commands)


def cmake_cache_inspect(step, ctx):
    p = step["params"]
    cache = os.path.join(ctx["root"], p.get("cache", "out/build/CMakeCache.txt"))
    if not os.path.isfile(cache):
        return outcome(0, {"present": False, "findings": []})
    wanted = ("CMAKE_HOME_DIRECTORY", "CMAKE_PREFIX_PATH", "CMAKE_CXX_COMPILER", "CMAKE_BUILD_TYPE",
              "CMAKE_GENERATOR")
    values = {}
    with open(cache, encoding="utf-8", errors="replace") as f:
        for line in f:
            key = line.split(":", 1)[0]
            if key in wanted and "=" in line:
                values[key] = line.split("=", 1)[1].strip()
    norm = lambda x: os.path.normcase(os.path.normpath(x))  # noqa: E731
    findings = []
    home = values.get("CMAKE_HOME_DIRECTORY")
    if home and norm(home) != norm(ctx["root"]):
        findings.append(f"cache was configured for a different source directory: {home}")
    prefix, expected = values.get("CMAKE_PREFIX_PATH"), p.get("expected_prefix")
    if prefix and expected and norm(prefix) != norm(expected):
        findings.append(f"cache Qt prefix {prefix} differs from the expected kit {expected}")
    return outcome(0, {"present": True, "values": values, "findings": findings})


def registry_snapshot(step, ctx):
    """Hash of an HKCU subtree, opened read-only. Values themselves are never stored."""
    if os.name != "nt":
        return outcome(0, {"supported": False})
    import winreg
    key_path = step["params"].get("key", "")
    if not key_path:
        return outcome(1, error="params.key is required")
    items = []

    def walk(path):
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ) as k:
            subs, vals, _ = winreg.QueryInfoKey(k)
            for i in range(vals):
                name, value, kind = winreg.EnumValue(k, i)
                items.append((path, name, kind, repr(value)))
            children = [winreg.EnumKey(k, i) for i in range(subs)]
        for c in children:
            walk(path + "\\" + c)
    try:
        walk(key_path)
    except FileNotFoundError:
        return outcome(0, {"supported": True, "present": False, "values": 0, "fingerprint": digest([])})
    except OSError as e:
        return outcome(1, error=f"registry read failed: {e}")
    data = {"supported": True, "present": True, "values": len(items), "fingerprint": digest(sorted(items))}
    base = step["params"].get("baseline")
    if base:
        old = ctx["results"].get(base, {}).get("data", {}).get("fingerprint")
        data["unchanged"] = old == data["fingerprint"] if old else None
    return outcome(0, data)
