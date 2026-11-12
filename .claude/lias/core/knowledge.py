"""LIAS knowledge store: verified project facts with provenance, FTS5 retrieval, audit trail."""
import hashlib
import json
import re
import time
from pathlib import Path

CATEGORIES = {"project_fact", "architecture_decision", "preference", "build_config",
              "validated_command", "known_error", "automation_meta", "temporary", "historical"}
# verified = confirmed by test/user; documented = from project docs, not re-confirmed;
# unverified = candidate; invalidated = retired.
STATUSES = {"verified", "documented", "unverified", "invalidated"}
CONFIDENCE = {"verified": 0.9, "documented": 0.6, "unverified": 0.3, "invalidated": 0.0}

SECRET_PATTERNS = [
    re.compile(r"(?i)\b(password|passwd|pwd|secret|api[_-]?key|access[_-]?key|auth[_-]?token|bearer)\b\s*[:=]\s*\S{4,}"),
    re.compile(r"\b(sk|pk|ghp|gho|xox[abp]|AKIA|AIza)[-_A-Za-z0-9]{16,}"),
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----"),
    re.compile(r"(?i)\b(bearer)\s+[A-Za-z0-9._~+/=-]{20,}"),
]


class SecretRejected(ValueError):
    pass


def contains_secret(text):
    return any(p.search(text) for p in SECRET_PATTERNS)


def _norm(s):
    return re.sub(r"\s+", " ", s.strip().lower())


def record_id(category, title):
    return hashlib.sha1(f"{category}|{_norm(title)}".encode()).hexdigest()[:12]


def _fts_query(q):
    terms = re.findall(r"[A-Za-z0-9_]{2,}", q)
    return " OR ".join(f'"{t}"' for t in terms[:12])


class Knowledge:
    def __init__(self, conn, project_id):
        self.c = conn
        self.pid = project_id

    # ---- internals
    def _audit(self, action, rid, detail=""):
        self.c.execute("INSERT INTO audit(ts,action,record_id,detail) VALUES(?,?,?,?)",
                       (time.time(), action, rid, detail[:500]))

    def _fts_set(self, rid, title, content):
        self.c.execute("DELETE FROM kfts WHERE id=?", (rid,))
        self.c.execute("INSERT INTO kfts(id,title,content) VALUES(?,?,?)", (rid, title, content))

    # ---- write
    def add(self, category, title, content, source, status="unverified", expires_days=None,
            related=None, supersede=False):
        if category not in CATEGORIES:
            raise ValueError(f"unknown category {category!r}")
        if status not in STATUSES or status == "invalidated":
            raise ValueError(f"bad status {status!r}")
        if not title.strip() or not content.strip() or not source.strip():
            raise ValueError("title, content and source are required (provenance)")
        if contains_secret(title) or contains_secret(content):
            raise SecretRejected("content looks like a credential/secret; refusing to store")
        if category == "temporary" and expires_days is None:
            expires_days = 7
        now = time.time()
        expires = now + expires_days * 86400 if expires_days else None
        rid = record_id(category, title)
        row = self.c.execute("SELECT * FROM knowledge WHERE id=?", (rid,)).fetchone()
        if row and row["status"] != "invalidated":
            if row["content"] == content:
                verified = now if status == "verified" else row["verified_at"]
                new_status = status if status == "verified" else row["status"]
                self.c.execute("UPDATE knowledge SET verified_at=?, status=?, confidence=?, expires_at=? WHERE id=?",
                               (verified, new_status, CONFIDENCE[new_status], expires or row["expires_at"], rid))
                self._audit("touch", rid, new_status)
                self.c.commit()
                return {"id": rid, "result": "unchanged"}
            if row["status"] == "verified" and not supersede:
                return {"id": rid, "result": "conflict",
                        "detail": "contradicts an existing verified record; pass supersede to replace it",
                        "existing": row["content"][:300]}
            self.c.execute("""UPDATE knowledge SET content=?, source=?, verified_at=?, status=?, confidence=?,
                              expires_at=?, version=version+1, related=? WHERE id=?""",
                           (content, source, now if status == "verified" else None, status,
                            CONFIDENCE[status], expires, json.dumps(related or []), rid))
            self._fts_set(rid, title, content)
            self._audit("update", rid, f"v{row['version'] + 1} {status} from {source}")
            self.c.commit()
            return {"id": rid, "result": "updated", "version": row["version"] + 1}
        if row:  # previously invalidated: revive as a fresh record
            self.c.execute("DELETE FROM knowledge WHERE id=?", (rid,))
        self.c.execute("""INSERT INTO knowledge(id,category,title,content,source,created,verified_at,status,
                          confidence,expires_at,project_id,related,version) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1)""",
                       (rid, category, title, content, source, now, now if status == "verified" else None,
                        status, CONFIDENCE[status], expires, self.pid, json.dumps(related or [])))
        self._fts_set(rid, title, content)
        self._audit("add", rid, f"{status} from {source}")
        self.c.commit()
        return {"id": rid, "result": "added"}

    def invalidate(self, rid, reason):
        row = self.c.execute("SELECT id FROM knowledge WHERE id=?", (rid,)).fetchone()
        if not row:
            return False
        self.c.execute("UPDATE knowledge SET status='invalidated', confidence=0 WHERE id=?", (rid,))
        self.c.execute("DELETE FROM kfts WHERE id=?", (rid,))
        self._audit("invalidate", rid, reason)
        self.c.commit()
        return True

    def delete(self, rid):
        n = self.c.execute("DELETE FROM knowledge WHERE id=?", (rid,)).rowcount
        self.c.execute("DELETE FROM kfts WHERE id=?", (rid,))
        if n:
            self._audit("delete", rid)
        self.c.commit()
        return bool(n)

    def expire_due(self):
        """Mark records past their expiry as invalidated. Returns count."""
        rows = self.c.execute("SELECT id FROM knowledge WHERE expires_at IS NOT NULL AND expires_at<? "
                              "AND status!='invalidated'", (time.time(),)).fetchall()
        for r in rows:
            self.invalidate(r["id"], "expired")
        return len(rows)

    # ---- read
    def get(self, rid):
        row = self.c.execute("SELECT * FROM knowledge WHERE id=?", (rid,)).fetchone()
        return dict(row) if row else None

    def _view(self, row, snippet):
        age = int((time.time() - (row["verified_at"] or row["created"])) // 86400)
        content = row["content"]
        if len(content) > snippet:
            content = content[:snippet].rstrip() + "..."
        return {"id": row["id"], "category": row["category"], "title": row["title"],
                "status": row["status"], "source": row["source"], "age_days": age,
                "version": row["version"], "content": content}

    def search(self, query, limit=5, max_chars=1500, category=None):
        """Exact title match first, then FTS5 bm25. Never returns invalidated/expired records."""
        now = time.time()
        out, seen, used = [], set(), 0
        per = max(200, max_chars // max(1, limit))

        def take(row):
            nonlocal used
            if row["id"] in seen or row["status"] == "invalidated":
                return
            if row["expires_at"] and row["expires_at"] < now:
                return
            if category and row["category"] != category:
                return
            if len(out) >= limit or used >= max_chars:
                return
            v = self._view(row, min(per, max_chars - used))
            seen.add(row["id"])
            used += len(v["content"])
            out.append(v)

        for row in self.c.execute("SELECT * FROM knowledge WHERE lower(title)=?", (_norm(query),)):
            take(row)
        fq = _fts_query(query)
        if fq:
            rows = self.c.execute("""SELECT k.* FROM kfts JOIN knowledge k ON k.id=kfts.id
                                     WHERE kfts MATCH ? ORDER BY bm25(kfts, 4.0, 1.0)
                                     LIMIT ?""", (fq, limit * 4)).fetchall()
            for row in rows:
                take(row)
        return out

    def list(self, status=None, limit=50):
        sql = "SELECT id,category,title,status,source,version FROM knowledge"
        args = []
        if status:
            sql += " WHERE status=?"
            args.append(status)
        sql += " ORDER BY created DESC LIMIT ?"
        args.append(limit)
        return [dict(r) for r in self.c.execute(sql, args)]

    # ---- markdown indexing (incremental)
    def index_markdown(self, directory):
        """Index memory/*.md (not history/) by '##' section as 'documented' records. Skips unchanged files."""
        directory = Path(directory)
        stats = {"files": 0, "skipped": 0, "records": 0, "removed": 0}
        for f in sorted(directory.glob("*.md")):
            text = f.read_text(encoding="utf-8", errors="replace")
            digest = hashlib.sha256(text.encode()).hexdigest()
            key = f"md:{f.name}"
            row = self.c.execute("SELECT value FROM meta WHERE key=?", (key,)).fetchone()
            if row and row[0] == digest:
                stats["skipped"] += 1
                continue
            prefix = f"memory/{f.name}#"
            old = self.c.execute("SELECT id FROM knowledge WHERE source LIKE ? AND category='project_fact' "
                                 "AND status='documented'", (prefix + "%",)).fetchall()
            for r in old:
                self.delete(r["id"])
                stats["removed"] += 1
            for heading, body in _sections(text):
                if len(body.strip()) < 40:
                    continue
                res = self.add("project_fact", f"{f.stem}: {heading}", body.strip()[:2500],
                               prefix + heading, status="documented")
                if res["result"] in ("added", "updated"):
                    stats["records"] += 1
            self.c.execute("INSERT OR REPLACE INTO meta VALUES(?,?)", (key, digest))
            self.c.commit()
            stats["files"] += 1
        return stats


def _sections(text):
    heading, buf = "intro", []
    for line in text.splitlines():
        m = re.match(r"^#{1,3}\s+(.*)", line)
        if m:
            if buf:
                yield heading, "\n".join(buf)
            heading, buf = m.group(1).strip(), []
        else:
            buf.append(line)
    if buf:
        yield heading, "\n".join(buf)
