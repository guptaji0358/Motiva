"""LIAS runner: validated, timeout-bounded, cached execution of registered automations."""
import hashlib
import json
import os
import subprocess
import sys
import time

MAX_OUTPUT = 1_000_000      # bytes of stdout kept
QUARANTINE_AFTER = 3        # consecutive failures
RUN_LOG_KEEP = 500
SKIP_DIRS = {".git", "build", "deployment", "deploy", "release", "third_party", "node_modules",
             "__pycache__", ".claude"}


def validate_args(manifest, args):
    spec = manifest["args"]
    unknown = set(args) - set(spec)
    if unknown:
        raise ValueError(f"unknown args: {sorted(unknown)}")
    out = {}
    for name, s in spec.items():
        if name in args:
            v = args[name]
        elif "default" in s:
            v = s["default"]
        elif s.get("required"):
            raise ValueError(f"missing required arg: {name}")
        else:
            continue
        t = s["type"]
        if t == "integer":
            if isinstance(v, bool) or not isinstance(v, int):
                raise ValueError(f"arg {name} must be an integer")
        elif t == "boolean":
            if not isinstance(v, bool):
                raise ValueError(f"arg {name} must be a boolean")
        elif not isinstance(v, str):
            raise ValueError(f"arg {name} must be a string")
        if t == "string" and ("\x00" in v or len(v) > 1000):
            raise ValueError(f"arg {name} invalid string")
        out[name] = v
    return out


def fingerprint(root, cache_spec):
    """Cheap change detector: names + mtime + size of files with the listed extensions."""
    exts = tuple(cache_spec.get("extensions", []))
    h = hashlib.sha256()
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in SKIP_DIRS)
        for f in sorted(files):
            if not exts or f.lower().endswith(exts):
                p = os.path.join(d, f)
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                h.update(f"{os.path.relpath(p, root)}|{st.st_mtime_ns}|{st.st_size}\n".encode())
    return h.hexdigest()


def _envelope(aid, status, started, result=None, error=None, cached=False, warnings=None):
    return {"automation_id": aid, "status": status, "duration_ms": int((time.time() - started) * 1000),
            "cached": cached, "result": result, "warnings": warnings or [], "error": error}


def run(paths, conn, registry, aid, args=None, allow_risk=1, use_cache=True):
    started = time.time()
    manifests, _ = registry.load()
    m = manifests.get(aid)
    if m is None:
        return _envelope(aid, "error", started, error={"code": "NOT_FOUND", "message": "no such automation"})
    status = registry.effective_status(m)
    if status != "active":
        return _envelope(aid, "error", started, error={"code": "NOT_ACTIVE", "message": f"status is {status}"})
    if m["risk"] > allow_risk:
        return _envelope(aid, "denied", started, error={
            "code": "RISK_DENIED", "message": f"risk tier {m['risk']} needs explicit authorization"})
    try:
        clean = validate_args(m, args or {})
    except ValueError as e:
        return _envelope(aid, "error", started, error={"code": "BAD_ARGS", "message": str(e)})

    cache_spec = m.get("cache") or {}
    key = None
    if use_cache and cache_spec.get("policy") == "fingerprint":
        fp = fingerprint(paths.root, cache_spec)
        key = hashlib.sha256(f"{aid}|{m['version']}|{json.dumps(clean, sort_keys=True)}|{fp}".encode()).hexdigest()
        row = conn.execute("SELECT result FROM cache WHERE key=? AND expires_at>?", (key, time.time())).fetchone()
        if row:
            env = _envelope(aid, "success", started, result=json.loads(row[0]), cached=True)
            _log(conn, env)
            return env

    cmd = [sys.executable, "-I", str(paths.scripts / m["entry"])]
    env_vars = dict(os.environ, LIAS_PROJECT_ROOT=str(paths.root))
    try:
        p = subprocess.run(cmd, input=json.dumps(clean), capture_output=True, text=True, encoding="utf-8",
                           errors="replace", timeout=m["timeout"], cwd=str(paths.root), env=env_vars)
        stdout = p.stdout[:MAX_OUTPUT]
        if p.returncode != 0:
            raise RuntimeError(f"exit {p.returncode}: {p.stderr.strip()[-400:]}")
        lines = [ln for ln in stdout.splitlines() if ln.strip()]
        result = json.loads(lines[-1]) if lines else None
        if not isinstance(result, dict):
            raise RuntimeError("script did not print a JSON object as its last line")
        env = _envelope(aid, "success", started, result=result)
    except subprocess.TimeoutExpired:
        env = _envelope(aid, "error", started, error={"code": "TIMEOUT", "message": f"exceeded {m['timeout']}s"})
    except (RuntimeError, json.JSONDecodeError) as e:
        env = _envelope(aid, "error", started, error={"code": "SCRIPT_FAILED", "message": str(e)})

    if env["status"] == "success":
        registry.set_state(aid, failures=0, last_run=time.time(), last_status="success")
        if key:
            ttl = int(cache_spec.get("ttl", 3600))
            conn.execute("INSERT OR REPLACE INTO cache VALUES(?,?,?,?,?)",
                         (key, aid, time.time(), time.time() + ttl, json.dumps(env["result"])))
            conn.commit()
    else:
        fails = registry.state(aid)["failures"] + 1
        registry.set_state(aid, failures=fails, last_run=time.time(), last_status="error")
        if fails >= QUARANTINE_AFTER:
            registry.set_state(aid, status="quarantined")
            env["warnings"].append(f"quarantined after {fails} consecutive failures")
    _log(conn, env)
    return env


def _log(conn, env):
    conn.execute("INSERT INTO runs(ts,automation_id,status,duration_ms,cached,error) VALUES(?,?,?,?,?,?)",
                 (time.time(), env["automation_id"], env["status"], env["duration_ms"], int(env["cached"]),
                  json.dumps(env["error"]) if env["error"] else None))
    conn.execute("DELETE FROM runs WHERE id NOT IN (SELECT id FROM runs ORDER BY id DESC LIMIT ?)", (RUN_LOG_KEEP,))
    conn.execute("DELETE FROM cache WHERE expires_at<?", (time.time(),))
    conn.commit()
