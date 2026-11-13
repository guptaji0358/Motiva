"""Shared helpers for LIAS git/build automations (not an automation itself)."""
import os
import shutil
import subprocess

TRAILER = "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
PR_FOOTER = "Generated with [Claude Code](https://claude.com/claude-code)"
SECRET_HINTS = (".env", "id_rsa", ".pem", ".pfx", "credentials", "secret", "token")


def tool(name, extra=()):
    p = shutil.which(name)
    if p:
        return p
    for d in extra:
        c = os.path.join(d, name + ".exe")
        if os.path.isfile(c):
            return c
    return None


def run(cmd, cwd, timeout=120, env=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout, env=env)
    return r.returncode, (r.stdout or "") + (r.stderr or "")
