"""LIAS automation registry: JSON manifests on disk + runtime state (quarantine) in SQLite."""
import json
import re

REQUIRED = {"id", "name", "description", "entry", "version", "risk", "timeout", "args", "status"}
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,48}$")
ARG_TYPES = {"string", "integer", "boolean"}
STATUS_ALLOWED = {"active", "inactive"}  # 'quarantined' only ever lives in DB state


class ManifestError(ValueError):
    pass


def validate_manifest(m, paths=None):
    missing = REQUIRED - set(m)
    if missing:
        raise ManifestError(f"missing keys: {sorted(missing)}")
    if not ID_RE.match(m["id"]):
        raise ManifestError("id must be lowercase letters, digits, hyphens")
    if m["status"] not in STATUS_ALLOWED:
        raise ManifestError("status must be active or inactive")
    if m["risk"] not in (0, 1, 2):
        raise ManifestError("risk must be 0, 1 or 2")
    if not isinstance(m["timeout"], int) or not 1 <= m["timeout"] <= 600:
        raise ManifestError("timeout must be 1..600 seconds")
    for name, spec in m["args"].items():
        if spec.get("type") not in ARG_TYPES:
            raise ManifestError(f"arg {name}: type must be one of {sorted(ARG_TYPES)}")
    entry = m["entry"]
    if "/" in entry or "\\" in entry or ".." in entry or not entry.endswith(".py"):
        raise ManifestError("entry must be a bare .py filename inside automation/scripts")
    if paths is not None and not (paths.scripts / entry).is_file():
        raise ManifestError(f"entry script not found: {entry}")
    return m


class Registry:
    def __init__(self, paths, conn):
        self.paths, self.c = paths, conn

    def load(self):
        """Return ({id: manifest}, {filename: error}). Bad manifests never break the registry."""
        good, bad = {}, {}
        if self.paths.manifests.is_dir():
            for f in sorted(self.paths.manifests.glob("*.json")):
                try:
                    m = validate_manifest(json.loads(f.read_text(encoding="utf-8")), self.paths)
                    if m["id"] in good:
                        raise ManifestError(f"duplicate id {m['id']}")
                    good[m["id"]] = m
                except (ManifestError, json.JSONDecodeError, OSError) as e:
                    bad[f.name] = str(e)
        return good, bad

    def state(self, aid):
        row = self.c.execute("SELECT * FROM automation_state WHERE id=?", (aid,)).fetchone()
        return dict(row) if row else {"id": aid, "status": None, "failures": 0, "last_run": None, "last_status": None}

    def effective_status(self, m):
        st = self.state(m["id"])["status"]
        return "quarantined" if st == "quarantined" else m["status"]

    def set_state(self, aid, **fields):
        self.c.execute("INSERT OR IGNORE INTO automation_state(id) VALUES(?)", (aid,))
        for k, v in fields.items():
            self.c.execute(f"UPDATE automation_state SET {k}=? WHERE id=?", (v, aid))
        self.c.commit()

    def release(self, aid):
        self.set_state(aid, status=None, failures=0)

    def summary(self):
        good, bad = self.load()
        items = [{"id": m["id"], "name": m["name"], "description": m["description"], "risk": m["risk"],
                  "version": m["version"], "status": self.effective_status(m),
                  "args": {k: v["type"] + ("*" if v.get("required") else "") for k, v in m["args"].items()}}
                 for m in good.values()]
        return items, bad
