"""Task manager storage: task records, checkpoints, locks, history and reusable evidence (plain JSON files)."""
import contextlib
import json
import os
import secrets
import time
from pathlib import Path

from .process import run_process
from .util import clean_env, digest, file_hash, now, read_json, write_json

QUEUED, RUNNING, BLOCKED, FAILED, COMPLETED, CANCELLED = (
    "queued", "running", "blocked", "failed", "completed", "cancelled")
EVIDENCE_MAX_AGE = 7 * 86400


class Busy(RuntimeError):
    pass


def pid_alive(pid):
    if pid <= 0:
        return False
    if os.name == "nt":  # os.kill(pid, 0) is NOT a probe on Windows (it signals the process)
        import ctypes
        k = ctypes.windll.kernel32
        k.OpenProcess.restype = ctypes.c_void_p
        handle = k.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not handle:
            return k.GetLastError() == 5  # access denied: it exists
        code = ctypes.c_ulong()
        got = k.GetExitCodeProcess(ctypes.c_void_p(handle), ctypes.byref(code))
        k.CloseHandle(ctypes.c_void_p(handle))
        return not got or code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        pass
    return True


def source_revision(root, evidence_free=(".claude/lias/agent-data", ".claude/lias/database")):
    """Fingerprint of HEAD plus the content of every changed/untracked file. 'no-git' outside a repository."""
    def git(*a):
        return run_process(["git", "--no-pager", "-c", "core.quotepath=false"] + list(a), root, 60,
                           clean_env({"GIT_OPTIONAL_LOCKS": "0"}))
    head = git("rev-parse", "HEAD")
    status = git("status", "--porcelain", "--untracked-files=all")
    if head["exit_code"] != 0 or status["exit_code"] != 0:
        return "no-git"
    files = []
    for line in status["output"].splitlines():
        rel = line[3:].split(" -> ")[-1].strip().strip('"')
        if not rel.startswith(evidence_free):
            files.append((line[:2], rel, file_hash(os.path.join(str(root), rel))))
    return digest([head["output"].strip(), files])


class Store:
    def __init__(self, paths):
        self.dir = paths.lias / "agent-data"
        self.tasks = self.dir / "tasks"
        self.evidence = self.dir / "evidence"

    # ---- locks
    @contextlib.contextmanager
    def lock(self, name, wait=0.0):
        """Exclusive lock file holding our PID. A lock whose owner process is gone is taken over."""
        self.dir.mkdir(parents=True, exist_ok=True)
        path = self.dir / f"{name}.lock"
        deadline = time.time() + wait
        while True:
            try:
                fd = os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
                os.write(fd, str(os.getpid()).encode())
                os.close(fd)
                break
            except (FileExistsError, PermissionError):
                try:
                    owner = int(path.read_text() or 0)
                    age = time.time() - path.stat().st_mtime
                except (OSError, ValueError):
                    owner, age = 0, 0
                # An empty lock is one being written right now unless it is old.
                if (owner and not pid_alive(owner)) or (not owner and age > 30):
                    with contextlib.suppress(OSError):
                        path.unlink()
                    continue
                if time.time() >= deadline:
                    raise Busy(f"'{name}' is locked by process {owner or '?'}")
                time.sleep(0.1)
        try:
            yield
        finally:
            with contextlib.suppress(OSError):
                path.unlink()

    # ---- tasks
    def task_dir(self, tid):
        if not tid or any(c not in "T0123456789abcdef-" for c in tid):
            raise ValueError(f"bad task id: {tid!r}")
        return self.tasks / tid

    def load(self, tid):
        f = self.task_dir(tid) / "task.json"
        if not f.is_file():
            raise ValueError(f"no such task: {tid}")
        return read_json(f)

    def save(self, task):
        task["updated"] = now()
        write_json(self.task_dir(task["id"]) / "task.json", task)

    def event(self, task, text):
        task["history"].append({"ts": now(), "event": text})

    def list(self):
        out = []
        for f in sorted(self.tasks.glob("T*/task.json")):
            try:
                out.append(read_json(f))
            except (OSError, json.JSONDecodeError):
                out.append({"id": f.parent.name, "state": "unreadable", "title": "corrupt task record"})
        return out

    def submit(self, spec, root, rerun=False):
        """Create a task, or return the existing one for the same spec on the same sources."""
        with self.lock("submit", wait=10):
            revision = source_revision(root)
            key = digest([{k: v for k, v in spec.items() if k not in ("grants", "max_parallel")}, revision])
            if not rerun:
                for t in reversed(self.list()):
                    if t.get("dedupe_key") == key and t.get("state") != CANCELLED:
                        return t, True
            tid = time.strftime("T%Y%m%d-%H%M%S-") + secrets.token_hex(3)
            task = {"id": tid, "title": spec["title"], "created": now(), "updated": now(), "state": QUEUED,
                    "spec": spec, "dedupe_key": key, "submitted_revision": revision, "revision": None,
                    "steps": {s["id"]: new_step_record() for s in spec["steps"]}, "approvals": {},
                    "history": [], "runs": 0}
            self.event(task, "submitted")
            self.save(task)
            return task, False

    # ---- reusable evidence
    def evidence_get(self, key):
        f = self.evidence / f"{key}.json"
        if not f.is_file():
            return None
        try:
            ev = read_json(f)
        except (OSError, json.JSONDecodeError):
            return None
        if time.time() - ev.get("at", 0) > EVIDENCE_MAX_AGE:
            return None
        # Evidence is only as good as the log that backs it.
        if not ev.get("log") or file_hash(ev["log"]) != ev.get("log_sha256"):
            return None
        return ev

    def evidence_put(self, key, task_id, record):
        log = record["verification"]["evidence"].get("log")
        if not log:
            return
        write_json(self.evidence / f"{key}.json",
                   {"at": time.time(), "when": now(), "task": task_id, "log": log, "log_sha256": file_hash(log),
                    "verification": record["verification"], "data": record["data"]})

    def diagnosis_history(self, diagnosis, task_id, fixed_by=None):
        """Remember each failure signature. Returns what was known before this occurrence."""
        f = self.dir / "diagnoses.json"
        try:
            known = read_json(f) if f.is_file() else {}
        except (OSError, json.JSONDecodeError):
            known = {}
        sig = diagnosis["signature"]
        before = dict(known[sig]) if sig in known else None
        entry = known.setdefault(sig, {"pattern": diagnosis["pattern"], "first_task": task_id, "count": 0,
                                       "fixed_by": None})
        entry["count"] += 0 if fixed_by else 1
        entry["fixed_by"] = fixed_by or entry["fixed_by"]
        write_json(f, known)
        return before


def new_step_record():
    return {"state": "pending", "attempts": [], "verification": None, "data": {}, "diagnoses": [],
            "note": None, "reused_from": None}
