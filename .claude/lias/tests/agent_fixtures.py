"""Throwaway project + scratch directory for agent tests. Nothing here touches the real repository."""
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from lias.agent.engine import Engine  # noqa: E402
from lias.core import db  # noqa: E402


def _force_remove(func, path, _):
    os.chmod(path, stat.S_IWRITE)  # git marks object files read-only
    func(path)


class AgentCase(unittest.TestCase):
    def setUp(self):
        base = Path(tempfile.mkdtemp(prefix="lias-agent-test-")).resolve()
        self.addCleanup(shutil.rmtree, base, onerror=_force_remove)
        self.root, self.scratch = base / "project", base / "scratch"
        self.root.mkdir()
        self.scratch.mkdir()
        self.write(".gitignore", ".claude/lias/agent-data/\nrun.log\n*.marker\n")
        self.git("init", "-q")
        self.commit()
        self.paths = db.Paths(self.root)
        self.engine = Engine(self.paths, retry_delay=0)

    # ---- project helpers
    def write(self, rel, text):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8", newline="\n")
        return p

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c",
                        "commit.gpgsign=false", "-c", "core.autocrlf=false"] + list(args),
                       cwd=self.root, check=True, capture_output=True)

    def commit(self):
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "t", "--allow-empty")

    # ---- task helpers
    def script(self, sid, code, **step):
        """A python_script step plus its script file."""
        rel = f"scripts/{sid}.py"
        self.write(rel, code)
        return {"id": sid, "action": "python_script", "params": {"script": rel},
                "expect": {"output_contains": ["OK"]}, **step}

    def submit(self, steps, grants=None, rerun=False, **spec):
        if grants is None:
            grants = [f"script:{s['params']['script']}" for s in steps if s["action"] == "python_script"]
        raw = {"title": "test task", "steps": steps, "grants": grants, **spec}
        return self.engine.submit(raw, str(self.scratch), rerun=rerun)

    def run_steps(self, steps, grants=None, **spec):
        task, _ = self.submit(steps, grants, **spec)
        return self.engine.run(task["id"])

    def state(self, task, sid):
        return task["steps"][sid]["state"]

    def report(self, task):
        return (self.engine.store.task_dir(task["id"]) / "report.md").read_text(encoding="utf-8")

    def runs_of(self, name="run.log"):
        f = self.root / name
        return f.read_text().split() if f.exists() else []


APPEND = "open('run.log','a').write('{0}\\n')\nprint('OK')\n"
