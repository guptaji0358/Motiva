"""Workflow observer: counts repeated shell commands and surfaces automation candidates (discovery only).

It records nothing about command output, never stores commands containing secrets, and does not generate code.
"""
import re
import time

from ..core.knowledge import contains_secret

IGNORE = re.compile(r"^(cd|ls|pwd|echo|cat|git (status|diff|log|add)|python[^ ]* [^ ]*lias[/\\]cli\.py)\b")
MAX_CMD = 300
MIN_REPEATS = 3


def normalize(cmd):
    return re.sub(r"\s+", " ", cmd.strip())[:MAX_CMD]


def record(conn, cmd):
    """Return True if the command was recorded."""
    cmd = normalize(cmd or "")
    if len(cmd) < 8 or IGNORE.match(cmd) or contains_secret(cmd):
        return False
    now = time.time()
    conn.execute("INSERT INTO observed(cmd,count,first_seen,last_seen) VALUES(?,1,?,?) "
                 "ON CONFLICT(cmd) DO UPDATE SET count=count+1,last_seen=excluded.last_seen", (cmd, now, now))
    conn.commit()
    return True


def candidates(conn, threshold=MIN_REPEATS, limit=10):
    """Commands repeated >= threshold times. Score = count * ~token cost of re-deriving (length proxy)."""
    rows = conn.execute("SELECT cmd,count,first_seen,last_seen FROM observed WHERE dismissed=0 AND count>=? "
                        "ORDER BY count DESC, last_seen DESC LIMIT ?", (threshold, limit)).fetchall()
    return [{"command": r["cmd"], "count": r["count"], "score": r["count"] * min(len(r["cmd"]), 120),
             "span_days": round((r["last_seen"] - r["first_seen"]) / 86400, 1)} for r in rows]


def dismiss(conn, cmd):
    n = conn.execute("UPDATE observed SET dismissed=1 WHERE cmd=?", (normalize(cmd),)).rowcount
    conn.commit()
    return bool(n)
