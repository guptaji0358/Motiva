"""LIAS command line. Usage: python .claude/lias/cli.py <command> ... (all output is compact JSON)."""
import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from lias.core import db, diagnostics, runner  # noqa: E402
from lias.core.knowledge import Knowledge, SecretRejected  # noqa: E402
from lias.core.registry import Registry  # noqa: E402

MAX_RESULTS, MAX_CHARS = 5, 1500


def out(obj):
    print(json.dumps(obj, ensure_ascii=False, separators=(",", ":")))


def build_parser():
    p = argparse.ArgumentParser(prog="lias")
    p.add_argument("--rebind", action="store_true", help="adopt a database from a moved/copied project")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("doctor")
    sub.add_parser("status")
    k = sub.add_parser("knowledge").add_subparsers(dest="kcmd", required=True)
    s = k.add_parser("search")
    s.add_argument("query")
    s.add_argument("--limit", type=int, default=MAX_RESULTS)
    s.add_argument("--max-chars", type=int, default=MAX_CHARS)
    s.add_argument("--category")
    a = k.add_parser("add")
    a.add_argument("category")
    a.add_argument("title")
    a.add_argument("content")
    a.add_argument("--source", required=True)
    a.add_argument("--status", default="unverified", choices=["verified", "documented", "unverified"])
    a.add_argument("--expires-days", type=int)
    a.add_argument("--supersede", action="store_true")
    g = k.add_parser("get")
    g.add_argument("id")
    ls = k.add_parser("list")
    ls.add_argument("--status")
    ls.add_argument("--limit", type=int, default=50)
    iv = k.add_parser("invalidate")
    iv.add_argument("id")
    iv.add_argument("--reason", required=True)
    dl = k.add_parser("delete")
    dl.add_argument("id")
    k.add_parser("index")
    k.add_parser("expire")
    au = sub.add_parser("automation").add_subparsers(dest="acmd", required=True)
    au.add_parser("list")
    r = au.add_parser("run")
    r.add_argument("id")
    r.add_argument("--args", default="{}", help="JSON object")
    r.add_argument("--no-cache", action="store_true")
    r.add_argument("--allow-risk", type=int, default=1, choices=[0, 1, 2])
    rl = au.add_parser("release")
    rl.add_argument("id")
    return p


def main(argv=None, paths=None):
    ns = build_parser().parse_args(argv)
    paths = paths or db.default_paths()
    try:
        conn = db.connect(paths, rebind=ns.rebind)
    except db.ProjectMismatch as e:
        out({"status": "error", "error": {"code": "PROJECT_MISMATCH", "message": str(e)}})
        return 2
    kn, reg = Knowledge(conn, paths.project_id), Registry(paths, conn)
    if ns.cmd == "doctor":
        r = diagnostics.doctor(paths, conn)
        out(r)
        return 0 if r["ok"] else 1
    if ns.cmd == "status":
        out(diagnostics.status(paths, conn))
    elif ns.cmd == "knowledge":
        c = ns.kcmd
        if c == "search":
            kn.expire_due()
            out(kn.search(ns.query, ns.limit, ns.max_chars, ns.category))
        elif c == "add":
            try:
                out(kn.add(ns.category, ns.title, ns.content, ns.source, ns.status, ns.expires_days,
                           supersede=ns.supersede))
            except SecretRejected as e:
                out({"result": "rejected", "detail": str(e)})
                return 3
            except ValueError as e:
                out({"result": "error", "detail": str(e)})
                return 2
        elif c == "get":
            out(kn.get(ns.id))
        elif c == "list":
            out(kn.list(ns.status, ns.limit))
        elif c == "invalidate":
            out({"invalidated": kn.invalidate(ns.id, ns.reason)})
        elif c == "delete":
            out({"deleted": kn.delete(ns.id)})
        elif c == "index":
            out(kn.index_markdown(paths.memory))
        elif c == "expire":
            out({"expired": kn.expire_due()})
    elif ns.cmd == "automation":
        if ns.acmd == "list":
            items, bad = reg.summary()
            out({"automations": items, "bad_manifests": bad})
        elif ns.acmd == "release":
            reg.release(ns.id)
            out({"released": ns.id})
        else:
            try:
                args = json.loads(ns.args)
                assert isinstance(args, dict)
            except (json.JSONDecodeError, AssertionError):
                out({"status": "error", "error": {"code": "BAD_ARGS", "message": "--args must be a JSON object"}})
                return 2
            env = runner.run(paths, conn, reg, ns.id, args, allow_risk=ns.allow_risk, use_cache=not ns.no_cache)
            out(env)
            return 0 if env["status"] == "success" else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
