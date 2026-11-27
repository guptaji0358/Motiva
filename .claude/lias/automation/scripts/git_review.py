"""Read-only final git review. One JSON line out; optional Markdown report in the system temp dir.

Input (all optional): {"base": "<ref>", "report": true, "remote": true}
base    compare base..HEAD (default: merge-base with origin/main, else main)
report  write the full result as Markdown to <temp>/motiva-git-review/ (never inside the repo)
remote  also ask origin for its branch heads (network; `git ls-remote`, nothing is fetched)

Never stages, commits, fetches, restores or deletes. Secret hits are reported as file:line only.
Pattern checks are heuristics: no hit is not proof that nothing is wrong.
"""
import json
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(HERE))))  # .claude, for `lias`

from _common import run, tool  # noqa: E402
from lias.core.knowledge import SECRET_PATTERNS  # noqa: E402

CAP = 40  # list items kept in the JSON; the report holds everything
EXTRA_SECRETS = [re.compile(p) for p in (r"\bgithub_pat_[A-Za-z0-9_]{30,}", r"\bhf_[A-Za-z0-9]{30,}",
                                         r"\bGOCSPX-[A-Za-z0-9_-]{20,}", r"\beyJ[A-Za-z0-9_-]{20,}\.eyJ")]
UNWANTED = re.compile(r"(?i)\.(pem|key|pfx|p12|exe|dll|obj|o|zip|db|sqlite3?|log|bak|tmp|orig|rej|pyc|lock)$"
                      r"|(^|/)(\.env[^/]*|desktop\.ini|rclone\.conf|motiva-cloud\.ini|settings\.local\.json)$")
# First match wins.
GROUPS = [("lias", r"^\.claude/(lias|agents|skills|settings)(/|\.|$)"),
          ("docs-memory", r"^(\.claude/memory/|docs/)|(^|/)[^/]*\.md$"),
          ("deploy-installer", r"^(scripts/(deploy|installer)/|build-and-deploy\.bat$|third_party/)"),
          ("tests", r"(^|/)tests/"),
          ("build", r"(^|/)CMakeLists\.txt$|^\.git(ignore|attributes)$|\.bat$"),
          ("source", r"^scripts/(src|include|main|shell|browser|extension|resources|tools)/"),
          ("website", r"^scripts/website/"),
          ("assets", r"^Assets/")]
WORDS = {w: n for n, w in enumerate("one two three four five six seven eight nine ten eleven twelve".split(), 1)}
SUITES = re.compile(r"(?i)\b(\d{1,2}|%s) (?:regression |test )?(?:suites|test executables)\b" % "|".join(WORDS))

root = os.environ["LIAS_PROJECT_ROOT"]
git_exe = tool("git")


def git(*args, ok=(0,), timeout=60):
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    code, text = run([git_exe, "-c", "core.quotepath=off", "-c", "core.safecrlf=false"] + list(args), root,
                     timeout, env)
    if code not in ok:
        raise RuntimeError("git %s: exit %d: %s" % (args[0], code, text.strip()[:300]))
    return text


def nul(*args):
    # stderr is merged into the output, so only use commands that stay quiet on success.
    return [x for x in git(*args).split("\0") if x]


def read(rel):
    try:
        with open(os.path.join(root, rel), "rb") as f:
            data = f.read(2_000_000)
    except OSError:
        return None
    return None if b"\0" in data else data.decode("utf-8", "replace")


def group_of(path):
    return next((name for name, pat in GROUPS if re.search(pat, path, re.I)), "other")


def working_tree():
    out = {"staged": [], "unstaged": [], "untracked": [], "conflicted": []}
    items = nul("status", "--porcelain", "-z", "--untracked-files=all")
    i = 0
    while i < len(items):
        xy, path = items[i][:2], items[i][3:]
        i += 2 if "R" in xy or "C" in xy else 1  # a rename is followed by its old path
        if xy == "??":
            out["untracked"].append(path)
        elif "U" in xy or xy in ("AA", "DD"):
            out["conflicted"].append(path)
        else:
            if xy[0] != " ":
                out["staged"].append("%s %s" % (xy[0], path))
            if xy[1] != " ":
                out["unstaged"].append("%s %s" % (xy[1], path))
    return out


def upstream_counts():
    """[ahead, behind] against the local tracking ref (not fetched), or None without an upstream."""
    text = git("rev-list", "--left-right", "--count", "HEAD...@{upstream}", ok=(0, 128)).split()
    return [int(x) for x in text] if len(text) == 2 and all(x.isdigit() for x in text) else None


def pick_base(requested):
    if requested:
        if requested.startswith("-"):
            raise RuntimeError("invalid base")
        return git("rev-parse", "--verify", requested + "^{commit}").strip(), requested
    for ref in ("origin/main", "main"):
        found = git("merge-base", ref, "HEAD", ok=(0, 1, 128)).strip()
        if re.fullmatch(r"[0-9a-f]{40,64}", found):
            return found, "merge-base with " + ref
    return None, None


def range_changes(base):
    items = nul("diff", "--name-status", "--no-renames", "-z", base, "HEAD")
    return list(zip(items[0::2], items[1::2]))


def secret_hits(files):
    hits = []
    for rel in files:
        text = read(rel)
        for n, line in enumerate((text or "").splitlines(), 1):
            if any(p.search(line) for p in SECRET_PATTERNS + EXTRA_SECRETS):
                hits.append("%s:%d" % (rel, n))
    return hits


def missing_qrc(tracked):
    known, out = set(tracked), []
    for q in (f for f in tracked if f.endswith(".qrc")):
        for entry in re.findall(r"<file[^>]*>([^<]+)</file>", read(q) or ""):
            target = os.path.normpath(os.path.join(os.path.dirname(q), entry)).replace("\\", "/")
            if target not in known:
                out.append("%s -> %s (not tracked)" % (q, target))
    return out


def deleted_references(deleted, tracked):
    """Tracked text files that still name a deleted file by its path or parent/name."""
    needles = {}
    for d in deleted:
        parts = d.split("/")
        needles[d] = d
        if len(parts) > 2:
            needles["/".join(parts[-2:])] = d
    # A shorter needle that also names an existing file would only produce noise.
    needles = {n: d for n, d in needles.items() if not any(t == n or t.endswith("/" + n) for t in tracked)}
    out = []
    for rel in tracked:
        if rel.startswith(".claude/memory/") or rel == ".gitignore":
            continue  # history and ignore rules legitimately name removed files
        text = read(rel) or ""
        found = sorted({d for n, d in needles.items() if n in text})
        out += ["%s mentions deleted %s" % (rel, d) for d in found]
    return out


def stale_notes(tracked):
    """Suite counts / test lists in agent notes that disagree with scripts/tests/CMakeLists.txt, memory limits."""
    out = []
    names = re.findall(r"(?:qt_)?add_executable\(\s*(\w+Tests)\b", read("scripts/tests/CMakeLists.txt") or "")
    agent_files = [f for f in tracked if re.match(r"\.claude/(agents/|lias/agent-teams/|lias/agent/workflows/)", f)]
    for rel in agent_files:
        for n, line in enumerate((read(rel) or "").splitlines(), 1):
            if not names:
                break
            for m in SUITES.finditer(line):
                said = WORDS.get(m.group(1).lower()) or int(m.group(1))
                if said != len(names):
                    out.append("%s:%d: says %d suites, CMake defines %d" % (rel, n, said, len(names)))
            listed = [t for t in names if re.search(r"\b%s\b" % t, line)]
            if len(listed) >= 3 and len(listed) < len(names):
                out.append("%s:%d: test list omits %s" % (rel, n, ", ".join(sorted(set(names) - set(listed)))))
    for rel in tracked:
        if re.match(r"\.claude/memory/[^/]+\.md$", rel):
            count = len((read(rel) or "").splitlines())
            if count > 200:
                out.append("%s: %d lines (limit 200)" % (rel, count))
    head_ref = re.search(r"HEAD `([0-9a-f]{7,40})`", read(".claude/memory/index.md") or "")
    if head_ref:
        behind = git("rev-list", "--count", head_ref.group(1) + "..HEAD", ok=(0, 128)).strip()
        if behind.isdigit() and int(behind):
            out.append(".claude/memory/index.md: cites HEAD %s, %s commit(s) behind (informational)"
                       % (head_ref.group(1), behind))
    return out


def review(a):
    tree = working_tree()
    pending = sorted({x.split(" ", 1)[1] for x in tree["staged"] + tree["unstaged"]}
                     | set(tree["untracked"]) | set(tree["conflicted"]))
    tracked = nul("ls-files", "-z")
    base, base_note = pick_base(a.get("base", "").strip())
    changes = range_changes(base) if base else []
    groups = {}
    for status, path in sorted(set(changes) | {("pending", p) for p in pending}, key=lambda c: c[1]):
        groups.setdefault(group_of(path), []).append("%s %s" % (status, path))
    # Content is scanned for every tracked and pending file as it is on disk (history is not scanned).
    secrets = secret_hits(sorted(set(tracked) | set(pending)))
    unwanted = sorted(f for f in set(tracked) | set(pending)  # a pending deletion of such a file is fine
                      if UNWANTED.search(f) and os.path.exists(os.path.join(root, f)))
    deleted = sorted({p for s, p in changes if s == "D"} | {x[2:] for x in tree["staged"] + tree["unstaged"]
                                                           if x.startswith("D ")})
    conflicts = git("grep", "--untracked", "-nIE", "^(<<<<<<<|>>>>>>>) ", ok=(0, 1)).split("\n")
    blocked = set(tree["conflicted"]) | set(unwanted) | {h.rsplit(":", 1)[0] for h in secrets} | {
        c.split(":", 1)[0] for c in conflicts if c}
    res = {
        "ok": True,
        "branch": git("rev-parse", "--abbrev-ref", "HEAD").strip(),
        "head": git("log", "-1", "--format=%h %s").strip(),
        "upstream_ahead_behind": upstream_counts(),
        "stashes": len(git("stash", "list").splitlines()),
        "working_tree": tree,
        "base": base[:12] if base else None, "base_source": base_note,
        "range": {"commits": git("log", "--format=%h %s", base + "..HEAD").splitlines() if base else [],
                  "files": len(changes),
                  "by_status": {s: sum(1 for c in changes if c[0] == s) for s in sorted({c[0] for c in changes})}},
        "groups": groups,
        "secret_hits": secrets,
        "unwanted_files": unwanted,
        "conflict_markers": [":".join(c.split(":", 2)[:2]) for c in conflicts if c],
        "missing_qrc_targets": missing_qrc(tracked),
        "deleted_references": deleted_references(deleted, tracked),
        "stale_notes": stale_notes(tracked),
        "candidates_to_commit": [p for p in pending if p not in blocked],
        "held_back": sorted(p for p in pending if p in blocked),
        "limits": "heuristic pattern checks of current files; git history is not scanned; nothing was built or tested",
    }
    if a.get("remote"):
        try:
            res["remote_heads"] = [x for x in git("ls-remote", "--heads", "origin", timeout=30).splitlines() if x]
        except Exception as e:  # network failure must not lose the local review
            res["remote_error"] = str(e)[:200]
    return res


def write_report(res):
    folder = os.path.join(tempfile.gettempdir(), "motiva-git-review")
    os.makedirs(folder, exist_ok=True)
    path = os.path.join(folder, "review-%s.md" % time.strftime("%Y%m%d-%H%M%S"))
    lines = ["# Git review: %s" % res["branch"], "", "HEAD: %s" % res["head"], ""]
    for key, value in res.items():
        lines += ["## " + key, "", "```json", json.dumps(value, indent=1), "```", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    return path


def capped(value):
    if isinstance(value, list):
        return value[:CAP] + (["... %d more (see report)" % (len(value) - CAP)] if len(value) > CAP else [])
    if isinstance(value, dict):
        return {k: capped(v) for k, v in value.items()}
    return value


def main():
    a = json.loads(sys.stdin.read() or "{}")
    try:
        if not git_exe:
            raise RuntimeError("git not found")
        res = review(a)
        path = write_report(res) if a.get("report") else None
        out = capped(res)
        if path:
            out["report_path"] = path
    except (RuntimeError, OSError) as e:
        out = {"ok": False, "error": str(e)[:500]}
    print(json.dumps(out))


if __name__ == "__main__":
    main()
