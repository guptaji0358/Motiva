"""Transaction-like file deployment with best-effort rollback (helper for deploy_local.py, not an automation).

Guarantees (accurate, not atomic):
- Every replacement is staged and verified (size + SHA-256) before any destination is touched.
- Each destination file is swapped with os.replace (atomic per file on one volume), but the SET of files is NOT
  applied atomically. A failure part-way triggers a best-effort rollback from backups taken before any change.
- Rollback restores overwritten files and removes only files/directories this run created. Unrelated files are
  never deleted.
- If the process is killed, loses power, or rollback itself fails, the destination may be mixed. In that case the
  stage directory (with backups and journal.json) is KEPT and reported so the previous versions can be restored by hand.
"""
import hashlib
import json
import os
import shutil
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class Deployment:
    def __init__(self, stage_root):
        self.root = os.path.normcase(os.path.abspath(stage_root))
        self.stage = os.path.join(stage_root, ".deploy-stage-%d-%d" % (os.getpid(), int(time.time())))
        self.items = []          # dicts: src, dst, staged, backup, existed, applied
        self.created_dirs = []   # directories created by this deployment, creation order
        self.changed = False     # True once the first destination was touched
        self.rollback_errors = []
        self.rolled_back = False

    # ---- phase 1+2: stage and back up (destinations untouched) ----
    def add(self, src, dst):
        d = os.path.normcase(os.path.abspath(dst))  # lexical containment: no '..' or absolute-path escape
        if not d.startswith(self.root + os.sep):
            raise OSError("destination escapes the deployment directory: %s" % dst)
        if os.path.isdir(dst):
            raise OSError("destination is a directory, expected a file: %s" % dst)
        self.items.append({"src": src, "dst": dst, "existed": os.path.isfile(dst), "applied": False,
                           "staged": None, "backup": None})

    def stage_all(self):
        os.makedirs(self.stage)
        for i, it in enumerate(self.items):
            staged = os.path.join(self.stage, "new", str(i))
            os.makedirs(os.path.dirname(staged), exist_ok=True)
            shutil.copyfile(it["src"], staged)
            if os.path.getsize(staged) != os.path.getsize(it["src"]) or sha256(staged) != sha256(it["src"]):
                raise OSError("staged copy does not match source: %s" % it["src"])
            it["staged"] = staged
            if it["existed"]:
                bak = os.path.join(self.stage, "backup", str(i))
                os.makedirs(os.path.dirname(bak), exist_ok=True)
                shutil.copy2(it["dst"], bak)
                if sha256(bak) != sha256(it["dst"]):
                    raise OSError("backup does not match destination: %s" % it["dst"])
                it["backup"] = bak
        self._journal()

    def _journal(self):
        data = [{"dst": it["dst"], "backup": it["backup"], "existed": it["existed"]} for it in self.items]
        with open(os.path.join(self.stage, "journal.json"), "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)

    # ---- phase 3: apply ----
    def apply(self):
        for it in self.items:
            d = os.path.dirname(it["dst"])
            missing = []
            while d and not os.path.isdir(d):
                missing.append(d)
                d = os.path.dirname(d)
            self.changed = True
            for m in reversed(missing):
                os.mkdir(m)
                self.created_dirs.append(m)
            os.replace(it["staged"], it["dst"])
            it["applied"] = True

    # ---- rollback ----
    def rollback(self):
        for it in reversed(self.items):
            if not it["applied"]:
                continue
            try:
                if it["existed"]:
                    tmp = it["dst"] + ".restore"
                    try:
                        shutil.copy2(it["backup"], tmp)
                        os.replace(tmp, it["dst"])
                    finally:
                        if os.path.exists(tmp):  # a failed restore must not leave a stray file behind
                            os.remove(tmp)
                else:
                    os.remove(it["dst"])
            except OSError as ex:
                self.rollback_errors.append("%s: %s" % (it["dst"], ex))
        for d in reversed(self.created_dirs):
            try:
                os.rmdir(d)  # only succeeds if empty, so unrelated content is never removed
            except OSError as ex:
                self.rollback_errors.append("%s: %s" % (d, ex))
        self.rolled_back = not self.rollback_errors
        return self.rolled_back

    # ---- cleanup ----
    def cleanup(self):
        """Remove staging. Kept (and path returned) when rollback failed, since it holds the only backups."""
        if self.rollback_errors:
            return self.stage
        shutil.rmtree(self.stage, ignore_errors=True)
        return None
