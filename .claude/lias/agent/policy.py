"""Permission policy: what runs automatically, what needs a grant, what needs a per-step approval.

This is a guardrail against mistakes, NOT a sandbox. Builds and tests execute project code, and
whoever can call the CLI can also call `approve`. Unknown things are classified as APPROVAL.
"""
import fnmatch
import os

from .util import is_secret_file, is_within

AUTO, SUPERVISED, APPROVAL = "automatic", "supervised", "approval"

OBSERVE_ACTIONS = {"git_snapshot", "git_compare", "process_check", "file_check", "cmake_cache_inspect",
                   "tool_check", "registry_snapshot", "source_snapshot"}
ACTIONS = OBSERVE_ACTIONS | {"cmake_configure", "cmake_build", "qt_test", "command", "python_script",
                             "lias_automation", "edit_file"}
# Grants a task may carry. "automation:<id>" and "script:<path>" are accepted as well.
FIXED_GRANTS = {"edit", "recovery", "visible-desktop"}
ALWAYS_APPROVAL_AUTOMATIONS = {"build-compile", "deploy-local", "installer-build", "git-commit-push",
                               "pr-create", "branch-create"}
GIT_READONLY = {"status", "diff", "log", "show", "rev-parse", "ls-files", "describe", "blame"}
GIT_UNSAFE_FLAGS = ("--output", "--ext-diff", "--textconv", "--open-files-in-pager")
VERSION_PROBES = {"git", "cmake", "ninja", "g++", "gcc", "python", "ctest"}
PROTECTED_WRITE = [".git/*", ".git", ".claude/settings*", ".claude/lias/agent/*",
                   ".claude/lias/agent-data/*", ".claude/lias/database/*"]


class PolicyError(ValueError):
    """The step can never run as written (not even with approval)."""


def _prog(argv):
    name = os.path.basename(str(argv[0])).lower()
    return name[:-4] if name.endswith(".exe") else name


def _unsafe_git_flag(arg):
    name = arg.split("=")[0]
    if arg.startswith(GIT_UNSAFE_FLAGS) or (arg.startswith("-O") and len(arg) > 2):
        return True
    # git accepts abbreviated long options, so "--outp=x" is "--output=x"
    return name.startswith("--") and len(name) > 2 and any(f.startswith(name) for f in GIT_UNSAFE_FLAGS)


def _trusted_tool(path, root, names):
    """A known tool name that does not come from inside the project."""
    if _prog([path]) not in names:
        return False
    return not os.path.dirname(str(path)) or not is_within(path, root)


def classify_command(argv):
    prog, rest = _prog(argv), [str(a) for a in argv[1:]]
    # Only a bare program name resolved through PATH can be automatic: a path could be anything.
    if os.path.dirname(str(argv[0])):
        return APPROVAL, "program given as a path"
    if prog in VERSION_PROBES and rest == ["--version"]:
        return AUTO, "version probe"
    if prog == "git":
        args = rest[1:] if rest[:1] == ["--no-pager"] else rest
        if args and args[0] in GIT_READONLY and not any(_unsafe_git_flag(a) for a in args):
            return AUTO, "read-only git"
        return APPROVAL, "git command that is not on the read-only list"
    return APPROVAL, f"command '{prog}' is not on the automatic list"


def classify(step, ctx):
    """ctx: {root, scratch, spec, manifests}. Returns {level, grant, reason}. Raises PolicyError."""
    action = step["action"]
    p = step.get("params", {})
    root, scratch = ctx["root"], ctx["scratch"]

    def res(level, reason, grant=None):
        return {"level": level, "reason": reason, "grant": grant}

    if action == "tool_check":
        if step.get("env", {}).get("set") or not all(
                _trusted_tool(t, root, VERSION_PROBES) for t in p.get("tools", {}).values()):
            return res(APPROVAL, "tool_check would run a program that is not a known toolchain tool")
    if action in OBSERVE_ACTIONS:
        return res(AUTO, "read-only inspection")
    if action in ("cmake_configure", "cmake_build"):
        build = p.get("build_dir")
        if not build:
            raise PolicyError("build_dir is required")
        if not _trusted_tool(p.get("cmake", "cmake"), root, {"cmake"}):
            return res(APPROVAL, "params.cmake is not a CMake executable outside the project")
        if action == "cmake_configure" and not is_within(p.get("source_dir", ""), root):
            return res(APPROVAL, "source directory is outside the project")
        extra = [str(a).lower() for a in p.get("args", [])]
        targets = [str(t).lower() for t in p.get("targets", [])]
        if any(a.startswith("--install") for a in extra) or any(
                w in t for t in targets for w in ("install", "deploy", "clean", "publish")):
            return res(APPROVAL, "install/deploy/clean target")
        if any(not a.startswith("-D") for a in p.get("args", [])):
            return res(APPROVAL, "non -D CMake arguments")
        if not is_within(build, scratch) or is_within(build, root):
            return res(APPROVAL, "build directory is not inside the scratch root")
        return res(AUTO, "build in scratch directory")
    if action == "qt_test":
        exe = p.get("exe")
        if not exe:
            raise PolicyError("exe is required")
        if not is_within(exe, scratch) or is_within(exe, root):
            return res(APPROVAL, "test executable is not a scratch build")
        if p.get("visible_desktop"):
            return res(SUPERVISED, "opens real windows on the user's desktop", "visible-desktop")
        return res(AUTO, "approved test from scratch build")
    if action == "command":
        argv = p.get("argv")
        if not isinstance(argv, list) or not argv or not all(isinstance(a, str) for a in argv):
            raise PolicyError("command needs params.argv as a non-empty list of strings (no shell strings)")
        level, reason = classify_command(argv)
        cwd = os.path.join(root, p.get("cwd", "."))
        if level == AUTO and (step.get("env") or not is_within(cwd, root)):
            level, reason = APPROVAL, "custom environment or working directory outside the project"
        return res(level, reason)
    if action == "python_script":
        path = os.path.join(root, p.get("script", ""))
        if not p.get("script") or not is_within(path, root) or not path.endswith(".py"):
            raise PolicyError("script must be a .py file inside the project")
        rel = os.path.relpath(path, root).replace("\\", "/")
        return res(SUPERVISED, "runs a project Python script", f"script:{rel}")
    if action == "lias_automation":
        aid = p.get("id")
        m = ctx["manifests"].get(aid)
        if m is None:
            raise PolicyError(f"unknown LIAS automation: {aid}")
        if aid in ALWAYS_APPROVAL_AUTOMATIONS or m["risk"] >= 2:
            return res(APPROVAL, f"automation {aid} publishes, deploys, changes git or kills processes")
        if m["risk"] == 1:
            return res(SUPERVISED, f"risk-1 automation {aid}", f"automation:{aid}")
        return res(AUTO, f"risk-0 automation {aid}")
    if action == "edit_file":
        rel = str(p.get("path", "")).replace("\\", "/")
        full = os.path.join(root, rel)
        if not rel or os.path.isabs(rel) or not is_within(full, root):
            raise PolicyError("edit path must be relative and inside the project")
        real = os.path.relpath(os.path.realpath(full), os.path.realpath(root)).replace("\\", "/")
        if is_secret_file(real) or any(fnmatch.fnmatch(real.lower(), g) for g in PROTECTED_WRITE):
            raise PolicyError(f"edit of protected path refused: {real}")
        allowed = ctx["spec"].get("allowed_write_paths", [])
        if not any(fnmatch.fnmatch(real, g) for g in allowed):
            raise PolicyError(f"{real} is not in the task's allowed_write_paths")
        return res(SUPERVISED, "modifies an approved source file", "edit")
    raise PolicyError(f"unknown action: {action}")
