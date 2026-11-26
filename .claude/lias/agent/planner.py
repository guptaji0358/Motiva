"""Planning engine: validate a task spec, expand a workflow, order steps, classify permissions."""
import os
import re
import tempfile
from pathlib import Path

from ..core.registry import Registry
from . import policy
from .util import digest, is_within, read_json, redact_obj

ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,48}$")
VAR_RE = re.compile(r"\$\{([a-z_]+(?:\.[A-Za-z0-9_]+)?)\}")
SPEC_KEYS = {"title", "task", "workflow", "vars", "steps", "grants", "allowed_write_paths", "scratch_root",
             "max_parallel"}
STEP_KEYS = {"id", "title", "action", "params", "depends_on", "run_after", "timeout", "retries", "resources",
             "expect", "inputs", "cacheable", "optional", "when_exists", "env"}
WORKFLOW_DIR = Path(__file__).resolve().parent / "workflows"
MAX_RETRIES = 3


class SpecError(ValueError):
    pass


def _expand(value, scope):
    if isinstance(value, dict):
        return {k: _expand(v, scope) for k, v in value.items()}
    if isinstance(value, list):
        out = []
        for v in value:
            e = _expand(v, scope)
            out.extend(e) if isinstance(v, str) and isinstance(e, list) else out.append(e)
        return out
    if isinstance(value, str):
        whole = VAR_RE.fullmatch(value)

        def look(name):
            if name not in scope:
                raise SpecError(f"unknown variable ${{{name}}}")
            return scope[name]
        if whole:
            return look(whole.group(1))
        return VAR_RE.sub(lambda m: str(look(m.group(1))), value)
    return value


def load_workflow(name):
    if not isinstance(name, str) or not ID_RE.match(name):
        raise SpecError("workflow name must be lowercase letters, digits, - or _")
    f = WORKFLOW_DIR / f"{name}.json"
    if not f.is_file():
        known = sorted(p.stem for p in WORKFLOW_DIR.glob("*.json"))
        raise SpecError(f"unknown workflow {name!r}; available: {known}")
    return read_json(f)


def normalise(raw, paths, scratch=None, extra_grants=()):
    """Return a fully expanded, validated spec (the thing that is stored and executed)."""
    if not isinstance(raw, dict):
        raise SpecError("task spec must be a JSON object")
    unknown = set(raw) - SPEC_KEYS
    if unknown:
        raise SpecError(f"unknown spec keys: {sorted(unknown)}")
    title = raw.get("title")
    if not isinstance(title, str) or not title.strip():
        raise SpecError("title is required")
    if bool(raw.get("workflow")) == bool(raw.get("steps")):
        raise SpecError("give exactly one of 'workflow' or 'steps'")
    wf = load_workflow(raw["workflow"]) if raw.get("workflow") else {}
    root = str(paths.root)
    scratch = os.path.abspath(scratch or raw.get("scratch_root") or os.environ.get("LIAS_AGENT_SCRATCH")
                              or os.path.join(tempfile.gettempdir(), f"lias-agent-{paths.project_id}"))
    if is_within(scratch, root) or is_within(root, scratch):
        raise SpecError("scratch root must be separate from the project directory")
    scope = {"root": root.replace("\\", "/"), "scratch": scratch.replace("\\", "/"),
             "project_id": paths.project_id}
    defaults = dict(wf.get("vars", {}))
    unknown_vars = set(raw.get("vars", {})) - set(defaults) if wf else set()
    if unknown_vars:
        raise SpecError(f"unknown workflow vars: {sorted(unknown_vars)}")
    defaults.update(raw.get("vars", {}))
    for k, v in defaults.items():  # a default may refer to ${root} / ${scratch} / earlier vars
        scope[f"vars.{k}"] = _expand(v, scope)
    steps = _expand(raw.get("steps") or wf.get("steps"), scope)
    grants = sorted(set(raw.get("grants", [])) | set(extra_grants))
    for g in grants:
        if g not in policy.FIXED_GRANTS and not g.startswith(("automation:", "script:")):
            raise SpecError(f"unknown grant {g!r} (approval-level actions cannot be granted in advance)")
    spec = {"title": title.strip(), "task": str(raw.get("task") or title).strip(), "workflow": raw.get("workflow"),
            "steps": _validate_steps(steps), "grants": grants,
            "allowed_write_paths": [str(x).replace("\\", "/") for x in raw.get("allowed_write_paths", [])],
            "scratch_root": scratch.replace("\\", "/"), "max_parallel": raw.get("max_parallel", 4)}
    if not isinstance(spec["max_parallel"], int) or not 1 <= spec["max_parallel"] <= 16:
        raise SpecError("max_parallel must be 1..16")
    if redact_obj(spec) != spec:  # stored records are redacted, which would corrupt the task
        raise SpecError("the task spec appears to contain a secret; secrets are never accepted or stored")
    return spec


def _validate_steps(steps):
    if not isinstance(steps, list) or not steps:
        raise SpecError("steps must be a non-empty list")
    seen, out = set(), []
    for s in steps:
        if not isinstance(s, dict):
            raise SpecError("each step must be an object")
        unknown = set(s) - STEP_KEYS
        if unknown:
            raise SpecError(f"unknown step keys: {sorted(unknown)}")
        sid = s.get("id")
        if not isinstance(sid, str) or not ID_RE.match(sid):
            raise SpecError(f"bad step id: {sid!r}")
        if sid in seen:
            raise SpecError(f"duplicate step id: {sid}")
        seen.add(sid)
        if s.get("action") not in policy.ACTIONS:
            raise SpecError(f"step {sid}: unknown action {s.get('action')!r}")
        step = {"id": sid, "title": s.get("title") or sid, "action": s["action"], "params": s.get("params", {}),
                "depends_on": s.get("depends_on", []), "run_after": s.get("run_after", []),
                "timeout": s.get("timeout", 120), "retries": s.get("retries", 0),
                "resources": s.get("resources", []), "expect": s.get("expect", {}), "inputs": s.get("inputs", []),
                "cacheable": s.get("cacheable", False), "optional": s.get("optional", False),
                "when_exists": s.get("when_exists", []), "env": s.get("env", {})}
        if not isinstance(step["timeout"], int) or not 1 <= step["timeout"] <= 3600:
            raise SpecError(f"step {sid}: timeout must be 1..3600")
        if not isinstance(step["retries"], int) or not 0 <= step["retries"] <= MAX_RETRIES:
            raise SpecError(f"step {sid}: retries must be 0..{MAX_RETRIES}")
        if step["cacheable"] and (not step["inputs"] or step["action"] not in ("qt_test", "command", "python_script")):
            raise SpecError(f"step {sid}: cacheable needs 'inputs' and is only for test/command/script steps "
                            "(builds are left to the build tool; observations always re-run)")
        out.append(step)
    for step in out:
        for dep in step["depends_on"] + step["run_after"]:
            if dep not in seen:
                raise SpecError(f"step {step['id']}: unknown dependency {dep}")
    waves(out)  # raises on cycles
    return out


def waves(steps):
    """Topological layers; steps in one layer have no dependency on each other."""
    left = {s["id"]: set(s["depends_on"]) | set(s["run_after"]) for s in steps}
    done, layers = set(), []
    while left:
        ready = [i for i, deps in left.items() if deps <= done]
        if not ready:
            raise SpecError(f"dependency cycle among: {sorted(left)}")
        layers.append(ready)
        done.update(ready)
        for i in ready:
            del left[i]
    return layers


def step_digest(step):
    """Identity of exactly what a human approves."""
    return digest({k: step[k] for k in ("id", "action", "params", "env", "timeout")})[:16]


def build_plan(spec, paths):
    manifests, _ = Registry(paths, None).load()  # manifests are files; no database needed
    ctx = {"root": str(paths.root), "scratch": spec["scratch_root"], "spec": spec, "manifests": manifests}
    layer_of = {sid: n for n, layer in enumerate(waves(spec["steps"])) for sid in layer}
    plan = []
    for s in spec["steps"]:
        try:
            perm = policy.classify(s, ctx)
        except policy.PolicyError as e:
            raise SpecError(f"step {s['id']}: {e}")
        needs = None
        if perm["level"] == policy.APPROVAL:
            needs = "approval"
        elif perm["level"] == policy.SUPERVISED and perm["grant"] not in spec["grants"]:
            needs = f"grant:{perm['grant']}"
        plan.append({"id": s["id"], "title": s["title"], "action": s["action"], "wave": layer_of[s["id"]],
                     "depends_on": s["depends_on"], "run_after": s["run_after"], "resources": s["resources"],
                     "permission": perm["level"], "grant": perm["grant"], "reason": perm["reason"],
                     "blocked_on": needs, "cacheable": s["cacheable"], "digest": step_digest(s)})
    plan.sort(key=lambda p: p["wave"])
    return plan


def render_plan(plan):
    lines, wave = [], None
    for p in plan:
        if p["wave"] != wave:
            wave = p["wave"]
            lines.append(f"Wave {wave + 1}:")
        deps = p["depends_on"] + p["run_after"]
        flags = [p["permission"]]
        if p["blocked_on"]:
            flags.append("NEEDS " + p["blocked_on"])
        if p["cacheable"]:
            flags.append("reusable evidence")
        if p["resources"]:
            flags.append("exclusive: " + ",".join(p["resources"]))
        lines.append(f"  {p['id']} [{p['action']}] ({'; '.join(flags)})"
                     + (f" <- {', '.join(deps)}" if deps else ""))
    return "\n".join(lines)
