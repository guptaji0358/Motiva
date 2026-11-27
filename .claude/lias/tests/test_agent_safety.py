"""Agent safety tests: permissions, approvals, destructive commands, secrets. Run as a file."""
import contextlib
import io
import json
import os
import sys
import unittest
from pathlib import Path
from unittest import mock

from agent_fixtures import APPEND, AgentCase

from lias.agent import exec_actions, planner, policy  # noqa: E402
from lias.agent.inspect_actions import outcome  # noqa: E402
from lias.agent.report import build_report  # noqa: E402
from lias.agent.util import clean_env, file_hash, redact  # noqa: E402

DESTRUCTIVE = [
    ["git", "commit", "-am", "x"], ["git", "push"], ["git", "reset", "--hard"], ["git", "stash"],
    ["git", "clean", "-fdx"], ["git", "checkout", "--", "."], ["git", "-c", "alias.status=!calc", "status"],
    ["git", "diff", "--output=keep.txt"], ["git", "log", "--outp=keep.txt"], ["rm", "-rf", "."], ["cmd", "/c", "del", "keep.txt"],
    ["powershell", "-Command", "Remove-Item keep.txt"], ["bash", "-c", "rm keep.txt"],
    ["taskkill", "/F", "/IM", "Motiva.exe"], ["reg", "add", "HKCU\\Software\\X"], ["C:/tools/git.exe", "status"],
    ["python", "-c", "print(1)"], ["curl", "https://example.invalid"], ["gh", "pr", "create"],
]


def command(argv, sid="c", **kw):
    return {"id": sid, "action": "command", "params": {"argv": argv}, **kw}


class PermissionTests(AgentCase):
    def level(self, step, **spec):
        task, _ = self.submit([step], grants=[], **spec)
        return self.engine.plan(task["id"])[1][0]

    def test_destructive_commands_need_approval(self):
        for argv in DESTRUCTIVE:
            with self.subTest(argv=argv):
                self.assertEqual(policy.classify_command(argv)[0], policy.APPROVAL)

    def test_read_only_commands_are_automatic(self):
        for argv in (["git", "status"], ["git", "--no-pager", "log", "-1"], ["cmake", "--version"]):
            self.assertEqual(policy.classify_command(argv)[0], policy.AUTO, argv)

    def test_automatic_actions_cannot_smuggle_programs(self):
        evil = str(self.root / "cmake.exe")
        build = str(self.scratch / "b")
        for step in (command(["git", "status"], env={"set": {"GIT_EXTERNAL_DIFF": "x"}}),
                     {"id": "t", "action": "tool_check", "params": {"tools": {"x": "C:/Windows/System32/cmd.exe"}}},
                     {"id": "t", "action": "tool_check", "params": {"tools": {"x": evil}}},
                     {"id": "b", "action": "cmake_build", "params": {"build_dir": build, "cmake": evil}},
                     {"id": "b", "action": "cmake_build", "params": {"build_dir": build, "cmake": "powershell"}}):
            with self.subTest(step=step):
                self.assertEqual(self.level(step)["permission"], policy.APPROVAL)

    def test_destructive_step_is_blocked_and_changes_nothing(self):
        keep = self.write("keep.txt", "precious")
        task = self.run_steps([command(["git", "clean", "-fdx"])], grants=["recovery", "edit"])
        self.assertEqual((task["state"], self.state(task, "c")), ("blocked", "blocked"))
        self.assertEqual(task["steps"]["c"]["attempts"], [])
        self.assertTrue(keep.exists())
        self.assertIn("## 9. Human decisions required\n- Approve `c`?", self.report(task))

    def test_approval_is_exact_and_single_use(self):
        task, _ = self.submit([command(["git", "gc"], expect={"exit_code_is_sufficient": True})], grants=[])
        digest = self.engine.plan(task["id"])[1][0]["digest"]
        with self.assertRaises(ValueError):
            self.engine.approve(task["id"], "c", "0" * 16)
        calls = []

        def fake(step, ctx):
            calls.append(1)
            return outcome(1, output="the file is being used by another process")
        with mock.patch.dict(exec_actions.REGISTRY, {"command": fake}):
            self.assertEqual(self.engine.run(task["id"])["state"], "blocked")
            self.assertEqual(calls, [])
            self.engine.approve(task["id"], "c", digest)
            self.assertEqual(self.engine.run(task["id"])["state"], "failed")
            self.assertEqual(len(calls), 1)  # never retried automatically
            self.assertEqual(self.engine.run(task["id"])["state"], "blocked")  # approval consumed
            self.assertEqual(len(calls), 1)

    def test_approval_never_carries_to_another_task(self):
        step = command(["git", "gc"], expect={"exit_code_is_sufficient": True})
        a, _ = self.submit([step], grants=[])
        self.engine.approve(a["id"], "c", self.engine.plan(a["id"])[1][0]["digest"])
        self.assertEqual(self.engine.run(a["id"])["state"], "completed")
        b = self.engine.run(self.submit([step], grants=[], rerun=True)[0]["id"])
        self.assertEqual(b["state"], "blocked")

    def test_supervised_step_needs_a_grant(self):
        step = self.script("s", "open('ran.marker','w')\nprint('OK')")
        task = self.run_steps([step], grants=[])
        self.assertEqual(task["state"], "blocked")
        self.assertFalse((self.root / "ran.marker").exists())
        self.engine.grant(task["id"], "script:scripts/s.py")
        self.assertEqual(self.engine.run(task["id"])["state"], "completed")
        with self.assertRaises(ValueError):
            self.engine.grant(task["id"], "push")

    def test_build_and_test_outside_scratch_need_approval(self):
        inside = {"id": "b", "action": "cmake_build", "params": {"build_dir": str(self.root / "out")}}
        self.assertEqual(self.level(inside)["permission"], policy.APPROVAL)
        ok = {"id": "b", "action": "cmake_build", "params": {"build_dir": str(self.scratch / "b")}}
        self.assertEqual(self.level(ok)["permission"], policy.AUTO)
        deploy = {"id": "b", "action": "cmake_build",
                  "params": {"build_dir": str(self.scratch / "b"), "targets": ["deploy"]}}
        self.assertEqual(self.level(deploy)["permission"], policy.APPROVAL)
        exe = {"id": "t", "action": "qt_test", "params": {"exe": str(self.root / "t.exe")}}
        self.assertEqual(self.level(exe)["permission"], policy.APPROVAL)

    def test_protected_and_unapproved_edits_are_refused_outright(self):
        for path in (".git/config", "../outside.txt", ".env", ".claude/settings.json",
                     ".claude/lias/agent/policy.py", "src/other.cpp"):
            step = {"id": "e", "action": "edit_file", "params": {"path": path, "old": "a", "new": "b"}}
            with self.subTest(path=path), self.assertRaises(planner.SpecError):
                self.submit([step], grants=["edit"], allowed_write_paths=["src/a.cpp", ".git/*", ".env", ".claude/*",
                                                                          "../*"])

    def test_approved_edit_is_applied_once_and_backed_up(self):
        target = self.write("src/a.cpp", "int x = 1;\n")
        step = {"id": "e", "action": "edit_file",
                "params": {"path": "src/a.cpp", "old": "x = 1", "new": "x = 2"}}
        task = self.run_steps([step], grants=["edit"], allowed_write_paths=["src/a.cpp"])
        self.assertEqual(task["state"], "completed")
        self.assertEqual(target.read_text(), "int x = 2;\n")
        self.assertEqual(Path(task["steps"]["e"]["data"]["backup"]).read_text(), "int x = 1;\n")
        self.assertIn("edited by the agent: `src/a.cpp`", self.report(task))
        again = self.run_steps([dict(step, id="e2")], grants=["edit"], allowed_write_paths=["src/a.cpp"])
        self.assertFalse(again["steps"]["e2"]["data"]["changed"])

    def test_git_change_is_detected(self):
        steps = [{"id": "before", "action": "git_snapshot"},
                 self.script("touch", "open('surprise.txt','w').write('x')\nprint('OK')", depends_on=["before"]),
                 {"id": "after", "action": "git_compare", "params": {"baseline": "before"},
                  "expect": {"unchanged": True}, "run_after": ["touch"]}]
        task = self.run_steps(steps)
        self.assertEqual(task["state"], "failed")
        self.assertEqual(task["steps"]["after"]["data"]["added"], ["surprise.txt"])


class SecretTests(AgentCase):
    def test_secret_environment_is_not_passed_to_children(self):
        with mock.patch.dict(os.environ, {"MY_API_KEY": "abc", "GH_TOKEN": "def", "NORMAL": "1"}):
            env = clean_env()
        self.assertNotIn("MY_API_KEY", env)
        self.assertNotIn("GH_TOKEN", env)
        self.assertEqual(env["NORMAL"], "1")

    def test_secrets_never_reach_logs_records_or_reports(self):
        secret = "ghp_" + "A1b2" * 8
        code = f"print('password = hunter2hunter2')\nprint('{secret[:6]}' + '{secret[6:]}')\nraise SystemExit(1)"
        task = self.run_steps([self.script("leak", code)])
        folder = self.engine.store.task_dir(task["id"])
        for f in folder.rglob("*"):
            if f.is_file():
                text = f.read_text(encoding="utf-8", errors="replace")
                self.assertNotIn(secret, text, f)
                self.assertNotIn("hunter2", text, f)
        self.assertIn("[REDACTED]", redact("api_key: abcdef123456"))

    def test_key_prefixes_need_a_separator(self):
        for word in ("skipped_dependency", "pkg_config_search_path"):
            self.assertEqual(redact(word), word)
        tail = "A1b2" * 5
        for prefix in ("sk-", "sk_live_", "pk_test_", "ghp_", "gho_", "xoxb-", "AKIA", "AIza"):
            self.assertEqual(redact(prefix + tail), "[REDACTED]", prefix)

    def test_spec_containing_a_secret_is_rejected(self):
        with self.assertRaises(planner.SpecError):
            self.submit([command(["git", "status"])], grants=[], task="use token=abcdef123456")

    def test_secret_file_content_is_not_read(self):
        key = self.write("dev_key.pem", "PRIVATE")
        self.assertTrue(file_hash(key).startswith("stat:"))


class VerificationTests(AgentCase):
    def test_exit_code_alone_is_not_completion(self):
        task = self.run_steps([command(["git", "status"])], grants=[])
        self.assertEqual((task["state"], self.state(task, "c")), ("blocked", "unverified"))

    def test_expectations_are_checked(self):
        bad = self.run_steps([command(["git", "status"], expect={"files": ["missing.bin"]})], grants=[])
        self.assertEqual(bad["state"], "failed")

    def _qt(self, text):
        exe = self.scratch / "T.exe"
        exe.write_text("x")
        step = {"id": "t", "action": "qt_test", "params": {"exe": str(exe)}}
        real_run = exec_actions.run_process

        def fake(argv, cwd, timeout, env, log_path=None, stdin_text=None):
            if str(argv[0]) != str(exe):
                return real_run(argv, cwd, timeout, env, log_path, stdin_text)
            if text is not None:
                with open(argv[2].rsplit(",", 1)[0], "w") as f:
                    f.write(text)
            return {"exit_code": 0, "timed_out": False, "error": None, "output": "", "duration_s": 0, "log": None}
        with mock.patch.object(exec_actions, "run_process", fake):
            return self.run_steps([step], grants=[])

    def test_qt_totals_are_required_and_judged(self):
        good = self._qt("Totals: 3 passed, 0 failed, 1 skipped, 0 blacklisted, 5ms\n")
        self.assertEqual(good["state"], "completed")
        self.assertIn("| **total (1 suites)** | | 3 | 0 | 1 |", self.report(good))
        for text in (None, "Totals: 0 passed, 0 failed, 3 skipped, 0 blacklisted, 5ms\n",
                     "FAIL!  : T::x() bad\nTotals: 2 passed, 1 failed, 0 skipped, 0 blacklisted, 5ms\n"):
            with self.subTest(text=text):
                self.write("n.txt", repr(text))
                self.assertEqual(self._qt(text)["state"], "failed")


class IncrementalTests(AgentCase):
    def setUp(self):
        super().setUp()
        self.write("input.txt", "v1")
        self.step = self.script("chk", "open('run.log','a').write('x\\n')\nprint('OK')",
                                cacheable=True, inputs=["input.txt", "scripts/chk.py"])

    def go(self, **kw):
        task, _ = self.submit([self.step], rerun=True)
        return self.engine.run(task["id"], **kw)

    def test_reuse_only_while_evidence_is_valid(self):
        first = self.go()
        second = self.go()
        self.assertEqual(self.state(second, "chk"), "reused")
        self.assertEqual(len(self.runs_of()), 1)
        self.assertIn(f"reused evidence of {first['id']}", self.report(second))
        self.go(fresh=True)
        self.assertEqual(len(self.runs_of()), 2)
        stamp = os.stat(self.root / "input.txt")
        self.write("input.txt", "v2")  # same size and timestamp, different content
        os.utime(self.root / "input.txt", ns=(stamp.st_atime_ns, stamp.st_mtime_ns))
        self.assertEqual(self.state(self.go(), "chk"), "verified")
        self.assertEqual(len(self.runs_of()), 3)

    def test_tampered_log_invalidates_evidence(self):
        log = self.go()["steps"]["chk"]["verification"]["evidence"]["log"]
        with open(log, "a") as f:
            f.write("edited")
        self.assertEqual(self.state(self.go(), "chk"), "verified")

    def test_failures_are_not_cached(self):
        self.write("scripts/chk.py", "open('run.log','a').write('x\\n')\nraise SystemExit(1)")
        self.go()
        self.go()
        self.assertEqual(len(self.runs_of()), 2)


class ReportTests(AgentCase):
    def test_report_has_all_ten_sections(self):
        text = self.report(self.run_steps([{"id": "g", "action": "git_snapshot"}], grants=[]))
        for n, title in enumerate(("Original task", "Actions performed", "Files changed", "Commands executed",
                                   "Tests and build results", "Failures encountered", "Fixes attempted",
                                   "Remaining problems", "Human decisions required",
                                   "Recommended next action"), 1):
            self.assertIn(f"## {n}. {title}", text)
        self.assertIn("- no grants\n- no approvals recorded\n", text)
        self.assertIn("No use of an approval is recorded", text)

    def test_report_states_approvals_and_grants_without_inventing_identity(self):
        task, _ = self.submit([{"id": "c", "action": "command", "params": {"argv": ["git", "gc"]},
                                "expect": {"output_contains": ["OK"]}}], grants=["edit"])
        task = self.engine.run(task["id"])
        self.assertIn("- no approvals recorded", self.report(task))
        digest = self.engine.plan(task["id"])[1][0]["digest"]
        self.engine.grant(task["id"], "recovery")
        self.engine.approve(task["id"], "c", digest, "first try")
        self.engine.approve(task["id"], "c", digest, "second try")
        pending = build_report(self.engine.store.load(task["id"]), self.plan_of(task), "logs")
        self.assertIn("not used so far; note: second try", pending)
        with mock.patch.dict(exec_actions.REGISTRY, {"command": lambda s, c: outcome(0, output="OK")}):
            done = self.engine.run(task["id"])
        text = self.report(done)
        self.assertIn("- grant `edit`: present at submission", text)
        self.assertIn("- grant `recovery`: added after submission at ", text)
        self.assertIn("replaced by a later approval before any recorded use; note: first try", text)
        self.assertIn("used by run 2; note: second try", text)
        self.assertIn("records no identity", text)
        self.assertIn("step(s) run under a recorded approval: c", text)

    def test_report_marks_an_interrupted_approval_use(self):
        task, _ = self.submit([{"id": "c", "action": "command", "params": {"argv": ["git", "gc"]}}], grants=[])
        self.engine.approve(task["id"], "c", self.plan_of(task)["c"]["digest"])

        def interrupt(step, ctx):
            raise KeyboardInterrupt
        with mock.patch.dict(exec_actions.REGISTRY, {"command": interrupt}):
            with self.assertRaises(KeyboardInterrupt):
                self.engine.run(task["id"])
        text = self.report(self.engine.run(task["id"]))
        self.assertIn("used by run 1, which was interrupted (outcome unknown)", text)
        self.assertIn("runs without a recorded end: 1", text)
        self.assertIn("| c | 1 | 0 | 0 | 1 |", text)
        self.assertNotIn("run under a recorded approval", text)

    def test_report_history_counts_resumes_and_reuse(self):
        self.write("input.txt", "1")
        flag = self.scratch / "go"
        steps = [self.script("a", APPEND.format("a"), cacheable=True, inputs=["input.txt", "scripts/a.py"]),
                 self.script("b", f"import os\nassert os.path.exists(r'{flag}')\nprint('OK')", depends_on=["a"])]
        task = self.run_steps(steps)
        first = self.report(task)
        self.assertIn("Runs started: 1 (resumes after the first: 0)", first)
        self.assertIn("(run 1) exit 0", first)
        self.assertIn("NOT MET: exit code: expected 0, got 1", first)
        flag.write_text("")
        self.write("source.txt", "edited between runs")
        text = self.report(self.engine.run(task["id"]))
        self.assertIn("Runs started: 2 (resumes after the first: 1); runs without a recorded end: 0", text)
        self.assertIn("| a | 1 | 1 | 1 | 0 |", text)
        self.assertIn("| b | 2 | 2 | 0 | 0 |", text)
        self.assertIn("(an earlier execution in this task is also recorded)", text)
        self.assertIn(f"`a` reused, checks recorded by task {task['id']}", text)
        self.assertIn("(run 2) exit 0", text)
        self.assertIn("met: output contains: expected OK, got found", text)

    def test_report_labels_stale_verification(self):
        task = self.run_steps([self.script("a", "raise SystemExit(1)")])
        task["steps"]["a"].update(state="blocked", note="x")
        text = build_report(task, self.plan_of(task), "logs")
        self.assertIn("checks from an earlier execution (result then: failed); not the current state", text)

    def plan_of(self, task):
        return {p["id"]: p for p in self.engine.plan(task["id"])[1]}


class WorkflowTests(AgentCase):
    def test_motiva_workflow_plan_is_harmless(self):
        task, _ = self.engine.submit({"title": "m", "workflow": "motiva-regression"}, str(self.scratch))
        plan = {p["id"]: p for p in self.engine.plan(task["id"])[1]}
        self.assertEqual([i for i, p in plan.items() if p["permission"] == policy.APPROVAL], [])
        self.assertEqual({i for i, p in plan.items() if p["blocked_on"]}, {"test-vd-window"})
        self.assertEqual(sum(p["action"] == "qt_test" for p in plan.values()), 7)
        self.assertEqual({p["action"] for p in plan.values()} - policy.OBSERVE_ACTIONS,
                         {"cmake_configure", "cmake_build", "qt_test"})

    def test_cli_submit_plan_do(self):
        spec = self.scratch / "task.json"
        spec.write_text(json.dumps({"title": "cli", "steps": [{"id": "g", "action": "git_snapshot"}]}))
        script = Path(__file__).resolve().parents[1] / "cli.py"
        sys.path.insert(0, str(script.parents[1]))
        from lias import cli
        for argv, code, needle in ((["agent", "do", str(spec), "--scratch", str(self.scratch)], 0, "COMPLETED"),
                                   (["agent", "do", str(spec), "--scratch", str(self.scratch)], 0, "Not executed again"),
                                   (["agent", "list"], 0, '"state":"completed"'),
                                   (["agent", "status", "T0"], 2, "no such task")):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(cli.main(argv, self.paths), code, buf.getvalue())
            self.assertIn(needle, buf.getvalue())


if __name__ == "__main__":
    unittest.main()
