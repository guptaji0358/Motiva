"""`python .claude/lias/cli.py agent <command>`: compact JSON out; reports are Markdown files."""
import argparse
import json
import sys

from ..core import db
from . import planner
from .engine import Engine
from .report import build_report
from .store import COMPLETED, QUEUED, Busy

# Exit codes the task owner can branch on.
EXIT = {"completed": 0, "queued": 0, "running": 0, "failed": 1, "blocked": 4, "cancelled": 1}


def build_parser():
    p = argparse.ArgumentParser(prog="lias agent")
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, helptext in (("submit", "store a task"), ("do", "submit, run and print the report")):
        s = sub.add_parser(name, help=helptext)
        src = s.add_mutually_exclusive_group(required=True)
        src.add_argument("spec", nargs="?", help="JSON task file, or - for stdin")
        src.add_argument("--workflow")
        s.add_argument("--task", help="original task wording (with --workflow)")
        s.add_argument("--var", action="append", default=[], metavar="NAME=VALUE")
        s.add_argument("--scratch")
        s.add_argument("--grant", action="append", default=[])
        s.add_argument("--rerun", action="store_true", help="new task even if an identical one exists")
        if name == "do":
            s.add_argument("--fresh", action="store_true")
    for name in ("run", "resume"):
        r = sub.add_parser(name)
        r.add_argument("id")
        r.add_argument("--fresh", action="store_true", help="do not reuse earlier evidence")
    for name in ("plan", "status", "report", "cancel"):
        sub.add_parser(name).add_argument("id")
    sub.add_parser("list")
    sub.add_parser("workflows")
    a = sub.add_parser("approve")
    a.add_argument("id")
    a.add_argument("step")
    a.add_argument("--digest", required=True)
    a.add_argument("--note", default="")
    g = sub.add_parser("grant")
    g.add_argument("id")
    g.add_argument("permission")
    return p


def out(obj):
    print(json.dumps(obj, ensure_ascii=False, separators=(",", ":")))


def _read_spec(ns):
    if ns.workflow:
        variables = {}
        for item in ns.var:
            name, sep, value = item.partition("=")
            if not sep:
                raise planner.SpecError(f"--var needs NAME=VALUE, got {item!r}")
            variables[name] = value
        raw = {"title": f"workflow: {ns.workflow}", "workflow": ns.workflow, "vars": variables}
        if ns.task:
            raw["task"] = ns.task
        return raw
    if ns.spec == "-":
        return json.loads(sys.stdin.read())
    with open(ns.spec, encoding="utf-8-sig") as f:
        return json.loads(f.read())


def _summary(engine, task):
    states = {sid: r["state"] for sid, r in task["steps"].items()}
    report = engine.store.task_dir(task["id"]) / "report.md"
    return {"id": task["id"], "state": task["state"], "title": task["title"], "steps": states,
            "report": str(report) if report.is_file() else None}


def main(argv=None, paths=None):
    ns = build_parser().parse_args(argv)
    paths = paths or db.default_paths()
    try:
        engine = Engine(paths)
        c = ns.cmd
        if c in ("submit", "do"):
            task, dup = engine.submit(_read_spec(ns), ns.scratch, ns.grant, ns.rerun)
            if c == "submit":
                out({"id": task["id"], "state": task["state"], "duplicate": dup,
                     "note": task.get("duplicate_note"), "next": f"agent plan {task['id']}"})
                return 0
            if dup and task["state"] != QUEUED:
                print(f"> Identical task {task['id']} already exists on unchanged sources "
                      f"(state {task['state']}). " + ("Not executed again.\n" if task["state"] == COMPLETED
                                                      else "Resuming it.\n"))
            task = engine.run(task["id"], fresh=ns.fresh)
            return _print_report(engine, task)
        if c in ("run", "resume"):
            task = engine.run(ns.id, fresh=ns.fresh)
            out(_summary(engine, task))
            return EXIT[task["state"]]
        if c == "plan":
            task, plan = engine.plan(ns.id)
            print(f"Task {task['id']}: {task['title']} [{task['state']}]")
            print(planner.render_plan(plan))
            for p in plan:
                if p["permission"] == "approval":
                    step = next(s for s in task["spec"]["steps"] if s["id"] == p["id"])
                    print(f"\nApproval needed for {p['id']}: {p['reason']}\n  params: "
                          f"{json.dumps(step['params'])}\n  digest: {p['digest']}")
        elif c == "status":
            out(_summary(engine, engine.store.load(ns.id)))
        elif c == "report":
            return _print_report(engine, engine.store.load(ns.id))
        elif c == "cancel":
            out(_summary(engine, engine.cancel(ns.id)))
        elif c == "list":
            out([{k: t.get(k) for k in ("id", "state", "title", "updated")} for t in engine.store.list()])
        elif c == "workflows":
            out({f.stem: planner.read_json(f).get("description", "")
                 for f in sorted(planner.WORKFLOW_DIR.glob("*.json"))})
        elif c == "approve":
            entry = engine.approve(ns.id, ns.step, ns.digest, ns.note)
            out({"approved": entry["id"], "digest": entry["digest"], "next": f"agent resume {ns.id}"})
        elif c == "grant":
            out({"grants": engine.grant(ns.id, ns.permission)["spec"]["grants"]})
        return 0
    except Busy as e:
        out({"error": "busy", "detail": str(e)})
        return 5
    except (ValueError, OSError) as e:  # includes SpecError and bad JSON
        out({"error": type(e).__name__, "detail": str(e)})
        return 2


def _print_report(engine, task):
    path = engine.store.task_dir(task["id"]) / "report.md"
    if path.is_file() and task["state"] != "cancelled":
        text = path.read_text(encoding="utf-8")
    else:
        plan = {p["id"]: p for p in planner.build_plan(task["spec"], engine.paths)}
        text = build_report(task, plan, str(path.parent / "logs"))
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")
    print(text)
    return EXIT[task["state"]]
