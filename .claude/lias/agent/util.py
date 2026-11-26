"""Shared helpers: atomic JSON, hashing, secret redaction, environment hygiene."""
import glob
import hashlib
import json
import os
import re
import time
from pathlib import Path

from ..core.knowledge import SECRET_PATTERNS

# Files whose *content* the agent never reads (not even to hash it).
SECRET_FILE_RE = re.compile(r"(?i)(^|[\\/])(\.env(\..*)?|id_rsa.*|id_ed25519.*|.*\.(pem|pfx|p12|key)|"
                            r"credentials?(\..*)?|secrets?\.(json|ya?ml|ini|txt|env))$")
SECRET_ENV_RE = re.compile(r"(?i)(token|secret|passw(or)?d|passwd|api_?key|access_?key|private_?key|credential)")
EXTRA_REDACT = [
    re.compile(r"-----BEGIN [A-Z ]*PRIVATE KEY-----.*?(-----END [A-Z ]*PRIVATE KEY-----|\Z)", re.S),
    re.compile(r"(?i)\b(authorization)\s*:\s*\S+(\s+\S+)?"),
    re.compile(r"://[^/\s:@]+:[^/\s@]+@"),
    # a token/secret assigned a long literal containing digits (not ordinary identifiers in source code)
    re.compile(r"(?i)\b\w*(token|secret)[\"']?\s*[:=]\s*[\"']?(?=[A-Za-z0-9_\-]*\d)[A-Za-z0-9_\-]{12,}"),
]
REDACTED = "[REDACTED]"


def redact(text):
    for pat in EXTRA_REDACT + SECRET_PATTERNS:
        text = pat.sub(REDACTED, text)
    return text


def redact_obj(obj):
    if isinstance(obj, str):
        return redact(obj)
    if isinstance(obj, dict):
        return {k: redact_obj(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [redact_obj(v) for v in obj]
    return obj


def is_secret_file(path):
    return bool(SECRET_FILE_RE.search(str(path)))


def clean_env(extra=None):
    """Child environment: inherited minus secret-looking variables, plus explicit additions."""
    env = {k: v for k, v in os.environ.items() if not SECRET_ENV_RE.search(k)}
    env.update({k: str(v) for k, v in (extra or {}).items()})
    return env


def now():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def digest(obj):
    return hashlib.sha256(json.dumps(obj, sort_keys=True, default=str).encode()).hexdigest()


def file_hash(path):
    """Content hash; secret-looking files are identified by size+mtime only."""
    try:
        if is_secret_file(path):
            st = os.stat(path)
            return f"stat:{st.st_size}:{st.st_mtime_ns}"
        h = hashlib.sha256()
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        return h.hexdigest()
    except OSError:
        return "missing"


def hash_patterns(root, patterns):
    """{relative path: content hash} for files matching glob patterns (relative to root or absolute)."""
    out = {}
    for pat in patterns:
        full = pat if os.path.isabs(pat) else os.path.join(str(root), pat)
        matches = [m for m in glob.glob(full, recursive=True)
                   if os.path.isfile(m) and "__pycache__" not in m]
        if not matches:
            out[pat] = "no-match"
        for m in sorted(matches):
            out[os.path.relpath(m, str(root)) if _same_drive(m, root) else m] = file_hash(m)
    return out


def _same_drive(a, b):
    return os.path.splitdrive(os.path.abspath(a))[0].lower() == os.path.splitdrive(os.path.abspath(str(b)))[0].lower()


def is_within(path, base):
    p = os.path.normcase(os.path.realpath(str(path)))
    b = os.path.normcase(os.path.realpath(str(base)))
    return p == b or p.startswith(b.rstrip("\\/") + os.sep)


def atomic_write_text(path, text):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(f"{path.name}.{os.getpid()}.tmp")
    tmp.write_text(text, encoding="utf-8")
    for attempt in range(10):  # antivirus / sync clients can briefly lock the target
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.05 * (attempt + 1))


def write_json(path, obj):
    atomic_write_text(path, json.dumps(redact_obj(obj), indent=1, ensure_ascii=False))


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))
