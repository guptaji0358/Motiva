"""Generate native Claude Code sub-agent definitions (.claude/agents/<name>.md). stdin: JSON args. stdout: one JSON line.

Native format (Claude Code): Markdown file with YAML frontmatter (name, description, tools, optional model)
and the system prompt as the body. This script never launches agents and never overwrites an existing file.

Actions: validate | create | team | list
  spec_file : project-relative JSON spec (single agent for create/validate)
  team_file : project-relative JSON team config {"team": str, "agents": [spec, ...]} (for team/validate)
  dry_run   : validate and report, write nothing
"""
import json
import os
import re
import sys
from pathlib import Path

NAME_RE = re.compile(r"^[a-z][a-z0-9-]{2,48}$")
ALL_TOOLS = {"Read", "Grep", "Glob", "Bash", "PowerShell", "Edit", "Write", "NotebookEdit", "WebFetch", "WebSearch"}
WRITE_TOOLS = {"Edit", "Write", "NotebookEdit"}
SHELL_TOOLS = {"Bash", "PowerShell"}
# role -> (default tools, tools it may never have)
ROLES = {
    "read-only": ({"Read", "Grep", "Glob"}, WRITE_TOOLS | SHELL_TOOLS),
    "verifier": ({"Read", "Grep", "Glob", "Bash", "PowerShell"}, WRITE_TOOLS),
    "implementation": ({"Read", "Grep", "Glob", "Bash", "PowerShell", "Edit", "Write"}, set()),
}
MODELS = {"inherit", "sonnet", "opus", "haiku"}
FORBIDDEN_TOOLS = {"Agent", "Task"}  # sub-agents must not spawn further agents
SECRET_HINTS = (".pem", ".pfx", ".key", ".env", "id_rsa", "credentials")
MAX_TEXT = 2000
MAX_ITEMS = 30


class SpecError(ValueError):
    pass


def safe_rel(root, rel, what):
    if not isinstance(rel, str) or not rel or "\x00" in rel:
        raise SpecError(f"{what}: invalid path")
    if os.path.isabs(rel) or re.match(r"^[A-Za-z]:", rel) or rel.startswith(("/", "\\")):
        raise SpecError(f"{what}: absolute paths not allowed: {rel}")
    parts = re.split(r"[\\/]+", rel)
    if ".." in parts:
        raise SpecError(f"{what}: path traversal not allowed: {rel}")
    full = (Path(root) / rel).resolve()
    if Path(root).resolve() not in full.parents and full != Path(root).resolve():
        raise SpecError(f"{what}: path escapes project root: {rel}")
    return full


def _text(spec, key, required=True):
    v = spec.get(key)
    if v is None or v == "":
        if required:
            raise SpecError(f"missing field: {key}")
        return ""
    if not isinstance(v, str) or "\x00" in v or len(v) > MAX_TEXT:
        raise SpecError(f"{key}: must be a string up to {MAX_TEXT} chars")
    return v.strip()


def _list(spec, key, required=False):
    v = spec.get(key, [])
    if not isinstance(v, list) or len(v) > MAX_ITEMS or not all(isinstance(i, str) and i.strip() and len(i) <= MAX_TEXT and "\x00" not in i for i in v):
        raise SpecError(f"{key}: must be a list of up to {MAX_ITEMS} non-empty strings")
    if required and not v:
        raise SpecError(f"{key}: must not be empty")
    return [i.strip() for i in v]


def validate_spec(spec, root):
    """Return a normalised spec or raise SpecError."""
    if not isinstance(spec, dict):
        raise SpecError("spec must be a JSON object")
    known = {"name", "description", "role", "objective", "instructions", "restrictions", "tools", "model", "owned_files"}
    extra = set(spec) - known
    if extra:
        raise SpecError(f"unknown fields: {sorted(extra)}")
    name = spec.get("name")
    if not isinstance(name, str) or not NAME_RE.match(name):
        raise SpecError("name must match ^[a-z][a-z0-9-]{2,48}$")
    role = spec.get("role")
    if role not in ROLES:
        raise SpecError(f"role must be one of {sorted(ROLES)}")
    defaults, banned = ROLES[role]
    tools = spec.get("tools")
    if tools is None:
        tools = sorted(defaults)
    else:
        tools = _list(spec, "tools", required=True)
    if len(set(tools)) != len(tools):
        raise SpecError("tools: duplicates not allowed")
    for t in tools:
        if t in FORBIDDEN_TOOLS:
            raise SpecError(f"tool {t} is forbidden (sub-agents must not spawn agents)")
        if t not in ALL_TOOLS:
            raise SpecError(f"invalid tool: {t}")
        if t in banned:
            raise SpecError(f"tool {t} not allowed for role {role}")
    model = spec.get("model", "inherit")
    if model not in MODELS:
        raise SpecError(f"model must be one of {sorted(MODELS)}")
    owned = _list(spec, "owned_files")
    if role == "implementation" and not owned:
        raise SpecError("implementation role requires owned_files (explicit file ownership)")
    if role != "implementation" and owned:
        raise SpecError("owned_files only valid for implementation role")
    for o in owned:
        safe_rel(root, o, "owned_files")
        if any(h in o.lower() for h in SECRET_HINTS):
            raise SpecError(f"owned_files: secret-looking path refused: {o}")
    desc = _text(spec, "description")
    if "\n" in desc:
        raise SpecError("description must be a single line")
    return {"name": name, "description": desc, "role": role, "objective": _text(spec, "objective"),
            "instructions": _list(spec, "instructions", required=True), "restrictions": _list(spec, "restrictions"),
            "tools": tools, "model": model, "owned_files": owned}


def _yaml_str(s):
    return json.dumps(s, ensure_ascii=False)  # JSON string is valid YAML double-quoted scalar


def render(s):
    fm = ["---", f"name: {s['name']}", f"description: {_yaml_str(s['description'])}", f"tools: {', '.join(s['tools'])}"]
    if s["model"] != "inherit":
        fm.append(f"model: {s['model']}")
    fm.append("---")
    body = [f"You are the `{s['name']}` specialist ({s['role']} role).", "", "## Objective", s["objective"], "",
            "## Instructions"] + [f"{i}. {t}" for i, t in enumerate(s["instructions"], 1)]
    rules = list(s["restrictions"])
    if s["role"] != "implementation":
        rules.insert(0, "Do not modify any file in the repository. Report findings; the orchestrator applies changes.")
    if s["role"] == "verifier":
        rules.append("Shell access is for running builds/tests/inspection only; never use it to write or delete project files.")
    rules.append("Do not spawn other agents. Never read, print, copy or log private keys (*.pem, *.pfx, .env).")
    rules.append("Do not commit, push, reset, rebase, stash or clean git state.")
    body += ["", "## Restrictions"] + [f"- {r}" for r in rules]
    if s["owned_files"]:
        body += ["", "## File ownership (edit only these)"] + [f"- `{o}`" for o in s["owned_files"]]
    body += ["", "## Report format",
             "Finish with a concise report: what you checked, exact evidence (file:line, command, output totals), "
             "confirmed defects vs unverified suspicions, and exact files changed (if any)."]
    return "\n".join(fm) + "\n\n" + "\n".join(body) + "\n"


def parse_frontmatter(text):
    m = re.match(r"^---\n(.*?)\n---\n", text, re.S)
    if not m:
        return None
    d = {}
    for line in m.group(1).splitlines():
        k, _, v = line.partition(":")
        d[k.strip()] = v.strip()
    return d


def check_rendered(s, text):
    """Post-render self check of the native frontmatter."""
    fm = parse_frontmatter(text)
    if not fm or fm.get("name") != s["name"] or not fm.get("description") or not fm.get("tools"):
        raise SpecError("rendered definition failed frontmatter check")
    if set(t.strip() for t in fm["tools"].split(",")) != set(s["tools"]):
        raise SpecError("rendered tools mismatch")


def load_json(root, rel, what):
    p = safe_rel(root, rel, what)
    if not p.is_file():
        raise SpecError(f"{what} not found: {rel}")
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise SpecError(f"{what} is not valid JSON: {e}")


def specs_from(root, args):
    if args.get("team_file"):
        team = load_json(root, args["team_file"], "team_file")
        if not isinstance(team, dict) or not isinstance(team.get("agents"), list) or not team["agents"]:
            raise SpecError("team_file must be {team, agents:[...]} with at least one agent")
        if len(team["agents"]) > 12:
            raise SpecError("team too large (max 12 agents)")
        specs = [validate_spec(a, root) for a in team["agents"]]
        names = [s["name"] for s in specs]
        dup = sorted({n for n in names if names.count(n) > 1})
        if dup:
            raise SpecError(f"duplicate agent names in team: {dup}")
        return specs
    if args.get("spec_file"):
        return [validate_spec(load_json(root, args["spec_file"], "spec_file"), root)]
    raise SpecError("spec_file or team_file required")


def agents_dir(root):
    return Path(root) / ".claude" / "agents"


def write_new(path, text):
    """Exclusive create: raises FileExistsError rather than overwrite."""
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "x", encoding="utf-8", newline="\n") as f:
        f.write(text)


def execute(root, args):
    action = args.get("action", "")
    dry = bool(args.get("dry_run", False))
    adir = agents_dir(root)
    if action == "list":
        found = []
        if adir.is_dir():
            for f in sorted(adir.glob("*.md")):
                fm = parse_frontmatter(f.read_text(encoding="utf-8", errors="replace")) or {}
                found.append({"name": fm.get("name", f.stem), "file": f.relative_to(root).as_posix(),
                              "tools": fm.get("tools", "")})
        return {"ok": True, "action": "list", "agents": found}
    if action not in ("validate", "create", "team"):
        raise SpecError("action must be validate, create, team or list")
    specs = specs_from(root, args)
    if action == "create" and len(specs) != 1:
        raise SpecError("create takes a single spec_file; use action=team for team_file")
    results, conflicts = [], []
    for s in specs:
        text = render(s)
        check_rendered(s, text)
        path = adir / f"{s['name']}.md"
        exists = path.exists()
        if exists:
            conflicts.append(s["name"])
        results.append({"name": s["name"], "role": s["role"], "tools": s["tools"],
                        "file": path.relative_to(root).as_posix(), "exists": exists, "_text": text, "_path": path})
    if action == "validate":
        status = "validated"
    elif conflicts:
        # all-or-nothing: refuse the whole batch if any target exists
        return {"ok": False, "error": "EXISTS", "message": f"refusing to overwrite existing agents: {conflicts}",
                "written": [], "conflicts": conflicts}
    elif dry:
        status = "dry-run"
    else:
        written = []
        try:
            for r in results:
                write_new(r["_path"], r["_text"])
                written.append(r["_path"])
        except OSError as e:  # roll back files created by this call only
            for p in written:
                try:
                    p.unlink()
                except OSError:
                    pass
            return {"ok": False, "error": "WRITE_FAILED", "message": str(e), "written": []}
        status = "written"
    return {"ok": True, "action": action, "status": status, "dry_run": dry,
            "agents": [{k: v for k, v in r.items() if not k.startswith("_")} for r in results]}


def main():
    args = json.loads(sys.stdin.read() or "{}")
    root = os.environ["LIAS_PROJECT_ROOT"]
    try:
        res = execute(root, args)
    except SpecError as e:
        res = {"ok": False, "error": "INVALID", "message": str(e)}
    # Always exit 0: rejected input is a normal result, and the LIAS runner quarantines scripts after 3 non-zero exits.
    print(json.dumps(res))


if __name__ == "__main__":
    main()
