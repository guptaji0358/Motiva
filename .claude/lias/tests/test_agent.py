"""Agent engine tests: tasks, scheduling, failures, recovery. Run: python .claude/lias/tests/test_agent.py"""
import time
import unittest
from pathlib import Path
from unittest import mock

from agent_fixtures import APPEND, AgentCase

from lias.agent import exec_actions, planner  # noqa: E402
from lias.agent.inspect_actions import outcome  # noqa: E402
from lias.agent.store import Busy, new_step_record  # noqa: E402

LOCK_ERROR = "ld: the file is being used by another process"


def scripted(*results):
    """A fake action returning the given (exit_code, output) pairs in order; the last one repeats."""
    calls = []

    def action(step, ctx):
        calls.append(ctx["env"])
        code, text = results[min(len(calls), len(results)) - 1]
        return outcome(code, output=text)
    action.calls = calls
    return action


def cmd(sid="c", **kw):
    return {"id": sid, "action": "command", "params": {"argv": ["git", "status"]},
            "expect": {"output_contains": ["OK"]}, **kw}


class TaskManagerTests(AgentCase):
    def test_invalid_specs_are_rejected(self):
        step = {"id": "a", "action": "git_snapshot"}
        bad = [
            {}, {"title": "t"}, {"title": "t", "steps": []}, {"title": "t", "steps": [step], "extra": 1},
            {"title": "t", "steps": [step, step]},
            {"title": "t", "steps": [{**step, "action": "magic"}]},
            {"title": "t", "steps": [{**step, "depends_on": ["zz"]}]},
            {"title": "t", "steps": [{**step, "depends_on": ["b"]}, {**step, "id": "b", "depends_on": ["a"]}]},
            {"title": "t", "steps": [{**step, "retries": 99}]},
            {"title": "t", "steps": [{**step, "cacheable": True, "inputs": ["x"]}]},
            {"title": "t", "steps": [step], "grants": ["git-push"]},
            {"title": "t", "workflow": "../evil"},
        ]
        for raw in bad:
            with self.subTest(raw=raw), self.assertRaises(planner.SpecError):
                self.engine.submit(raw, str(self.scratch))
        with self.assertRaises(planner.SpecError):
            self.engine.submit({"title": "t", "steps": [step]}, str(self.root / "inside"))

    def test_ids_are_unique_and_records_persist(self):
        a, _ = self.submit([{"id": "a", "action": "git_snapshot"}])
        b, _ = self.submit([{"id": "a", "action": "git_snapshot"}], rerun=True)
        self.assertNotEqual(a["id"], b["id"])
        loaded = type(self.engine)(self.paths).store.load(a["id"])
        self.assertEqual(loaded["state"], "queued")
        self.assertEqual({t["id"] for t in self.engine.store.list()}, {a["id"], b["id"]})
        with self.assertRaises(ValueError):
            self.engine.store.load("../../x")

    def test_duplicate_submission_and_execution_are_prevented(self):
        steps = [self.script("once", APPEND.format("once"))]
        first = self.run_steps(steps)
        again, duplicate = self.submit(steps)
        self.assertTrue(duplicate)
        self.assertEqual(again["id"], first["id"])
        self.engine.run(first["id"])
        self.assertEqual(self.runs_of(), ["once"])
        self.write("new.txt", "sources changed")
        changed, duplicate = self.submit(steps)
        self.assertFalse(duplicate)
        self.assertNotEqual(changed["id"], first["id"])

    def test_project_run_lock(self):
        task, _ = self.submit([{"id": "a", "action": "git_snapshot"}])
        with self.engine.store.lock("run"):
            with self.assertRaises(Busy):
                self.engine.run(task["id"])
        (self.engine.store.dir / "run.lock").write_text("999999999")  # owner process is gone
        self.assertEqual(self.engine.run(task["id"])["state"], "completed")

    def test_cancelled_task_does_not_run(self):
        task, _ = self.submit([self.script("a", APPEND.format("a"))])
        self.engine.cancel(task["id"])
        self.assertEqual(self.engine.run(task["id"])["state"], "cancelled")
        self.assertEqual(self.runs_of(), [])


class SchedulingTests(AgentCase):
    def test_dependency_order(self):
        steps = [self.script("c", APPEND.format("c"), depends_on=["b"]),
                 self.script("b", APPEND.format("b"), depends_on=["a"]),
                 self.script("a", APPEND.format("a"))]
        self.assertEqual(self.run_steps(steps)["state"], "completed")
        self.assertEqual(self.runs_of(), ["a", "b", "c"])

    def test_failed_dependency_skips_dependents_but_runs_after_steps_still_run(self):
        steps = [self.script("bad", "raise SystemExit(3)"),
                 self.script("child", APPEND.format("child"), depends_on=["bad"]),
                 self.script("always", APPEND.format("always"), run_after=["bad", "child"])]
        task = self.run_steps(steps)
        self.assertEqual(task["state"], "failed")
        self.assertEqual(self.state(task, "child"), "skipped_dependency")
        self.assertEqual(self.runs_of(), ["always"])

    def test_independent_steps_run_in_parallel(self):
        # Each script succeeds only if the other one is running at the same time.
        code = ("import os,time\nopen('{me}.marker','w').close()\nend=time.time()+20\n"
                "while not os.path.exists('{other}.marker'):\n    assert time.time()<end\n    time.sleep(0.05)\n"
                "print('OK')\n")
        steps = [self.script("x", code.format(me="x", other="y"), timeout=30),
                 self.script("y", code.format(me="y", other="x"), timeout=30)]
        self.assertEqual(self.run_steps(steps)["state"], "completed")

    def test_shared_resource_serialises_steps(self):
        code = ("import os,time\nfd=os.open('busy.marker',os.O_CREAT|os.O_EXCL)\ntime.sleep(0.4)\n"
                "os.close(fd)\nos.remove('busy.marker')\nprint('OK')\n")
        steps = [self.script(n, code, resources=["port"]) for n in ("p", "q", "r")]
        self.assertEqual(self.run_steps(steps)["state"], "completed")

    def test_irrelevant_step_is_skipped(self):
        task = self.run_steps([self.script("a", APPEND.format("a"), when_exists=["no/such/file"])])
        self.assertEqual((task["state"], self.state(task, "a")), ("completed", "not_applicable"))
        self.assertEqual(self.runs_of(), [])


class FailureTests(AgentCase):
    def test_failed_command_is_recorded(self):
        task = self.run_steps([self.script("bad", "print('boom')\nraise SystemExit(7)")])
        rec = task["steps"]["bad"]
        self.assertEqual((task["state"], rec["state"]), ("failed", "failed"))
        self.assertEqual(rec["attempts"][0]["exit_code"], 7)
        self.assertIn("boom", Path(rec["verification"]["evidence"]["log"]).read_text())
        self.assertIn("exit 7", self.report(task))

    def test_timeout_kills_the_command(self):
        code = "import time\ntime.sleep(6)\nopen('late.marker','w').close()\nprint('OK')\n"
        started = time.time()
        task = self.run_steps([self.script("slow", code, timeout=1)])
        self.assertLess(time.time() - started, 15)
        self.assertEqual(task["steps"]["slow"]["diagnoses"][0]["pattern"], "timeout")
        self.assertEqual(task["state"], "failed")
        time.sleep(6.5)
        self.assertFalse((self.root / "late.marker").exists())

    def test_missing_program_is_a_failure(self):
        task = self.run_steps([{"id": "g", "action": "tool_check",
                                "params": {"tools": {"cmake": str(self.scratch / "nope" / "cmake.exe")}}}])
        self.assertEqual(task["state"], "failed")

    def test_internal_error_never_becomes_success(self):
        with mock.patch.dict(exec_actions.REGISTRY, {"command": mock.Mock(side_effect=RuntimeError("x"))}):
            task = self.run_steps([cmd()], grants=[])
        self.assertEqual(task["state"], "failed")

    def _transient(self, grants, retries, *results):
        fake = scripted(*results)
        with mock.patch.dict(exec_actions.REGISTRY, {"command": fake}):
            task = self.run_steps([cmd(retries=retries)], grants=grants)
        return task, len(fake.calls)

    def test_transient_failure_is_retried_and_verified(self):
        task, calls = self._transient(["recovery"], 2, (1, LOCK_ERROR), (0, "OK"))
        self.assertEqual((task["state"], calls), ("completed", 2))
        self.assertEqual(task["steps"]["c"]["diagnoses"][0]["fix"]["result"], "verified")
        self.assertIn("retry for file-locked -> verified", self.report(task))

    def test_retries_are_bounded(self):
        task, calls = self._transient(["recovery"], 2, (1, LOCK_ERROR))
        self.assertEqual((task["state"], calls), ("failed", 3))
        self.assertIn("exhausted", task["steps"]["c"]["diagnoses"][-1]["stopped"])

    def test_no_retry_without_recovery_grant(self):
        task, calls = self._transient([], 3, (1, LOCK_ERROR))
        self.assertEqual((task["state"], calls), ("failed", 1))

    def test_unrecognised_failure_is_not_retried(self):
        task, calls = self._transient(["recovery"], 3, (1, "something odd"))
        self.assertEqual(calls, 1)
        self.assertIn("Unknown failure", self.report(task))

    def test_compile_error_is_escalated(self):
        task, calls = self._transient(["recovery"], 3, (1, "a.cpp:3:5: error: 'x' was not declared"))
        self.assertEqual(calls, 1)
        self.assertEqual(task["steps"]["c"]["diagnoses"][0]["pattern"], "compile-or-link-error")

    def test_environment_fix_is_tried_once(self):
        tools = self.scratch / "bin"
        tools.mkdir()
        step = cmd()
        step["params"]["recovery_path"] = [str(tools)]
        fake = scripted((0xC0000135, ""))
        with mock.patch.dict(exec_actions.REGISTRY, {"command": fake}):
            # a command is approval-level unless read-only git, which this is
            task = self.run_steps([step], grants=["recovery"])
        self.assertEqual(len(fake.calls), 2)
        self.assertTrue(any(str(tools) in v for v in fake.calls[1].values()))
        self.assertEqual(task["state"], "failed")

    def test_repeat_failure_is_recognised(self):
        self._transient([], 0, (1, "odd"))
        self.write("x.txt", "1")
        task, _ = self._transient([], 0, (1, "odd"))
        self.assertIsNotNone(task["steps"]["c"]["diagnoses"][0]["seen_before"])


class ResumeTests(AgentCase):
    def test_resume_does_not_repeat_verified_work(self):
        flag = self.scratch / "go"
        steps = [self.script("a", APPEND.format("a")),
                 self.script("b", f"import os\nassert os.path.exists(r'{flag}')\nprint('OK')", depends_on=["a"])]
        task = self.run_steps(steps)
        self.assertEqual(task["state"], "failed")
        flag.write_text("")
        task = self.engine.run(task["id"])
        self.assertEqual(task["state"], "completed")
        self.assertEqual(self.runs_of(), ["a"])

    def test_dependent_of_blocked_step_runs_after_approval(self):
        steps = [cmd("a", params={"argv": ["git", "gc"]}),
                 self.script("b", APPEND.format("b"), depends_on=["a"])]
        task = self.run_steps(steps)
        saved = self.engine.store.load(task["id"])
        self.assertEqual((self.state(saved, "a"), self.state(saved, "b")), ("blocked", "skipped_dependency"))
        self.engine.approve(task["id"], "a", self.engine.plan(task["id"])[1][0]["digest"])
        with mock.patch.dict(exec_actions.REGISTRY, {"command": scripted((0, "OK"))}):
            done = self.engine.run(task["id"])
        self.assertEqual((done["state"], self.state(done, "b")), ("completed", "verified"))
        self.assertEqual(self.runs_of(), ["b"])

    def test_changed_sources_invalidate_checkpoints(self):
        steps = [self.script("a", APPEND.format("a")), self.script("b", "raise SystemExit(1)", depends_on=["a"])]
        task = self.run_steps(steps)
        self.write("source.txt", "edited between runs")
        self.engine.run(task["id"])
        self.assertEqual(self.runs_of(), ["a", "a"])

    def test_recovers_from_interruption(self):
        steps = [self.script("a", APPEND.format("a")), self.script("b", APPEND.format("b"), depends_on=["a"])]
        task, _ = self.submit(steps)
        real = exec_actions.REGISTRY["python_script"]

        def interrupt(step, ctx):
            if step["id"] == "b":
                raise KeyboardInterrupt
            return real(step, ctx)
        with mock.patch.dict(exec_actions.REGISTRY, {"python_script": interrupt}):
            with self.assertRaises(KeyboardInterrupt):
                self.engine.run(task["id"])
        saved = self.engine.store.load(task["id"])
        self.assertEqual((saved["state"], self.state(saved, "a"), self.state(saved, "b")),
                         ("running", "verified", "running"))
        done = self.engine.run(task["id"])
        self.assertEqual(done["state"], "completed")
        self.assertEqual(self.runs_of(), ["a", "b"])

    def test_interrupted_approval_step_needs_new_approval(self):
        task, _ = self.submit([cmd(params={"argv": ["git", "gc"]})], grants=[])
        digest = self.engine.plan(task["id"])[1][0]["digest"]
        self.engine.approve(task["id"], "c", digest)
        saved = self.engine.store.load(task["id"])
        saved["state"] = "running"
        saved["steps"]["c"] = dict(new_step_record(), state="running")
        saved["approvals"]["c"]["used"] = True
        self.engine.store.save(saved)
        fake = scripted((0, "OK"))
        with mock.patch.dict(exec_actions.REGISTRY, {"command": fake}):
            done = self.engine.run(task["id"])
        self.assertEqual((done["state"], len(fake.calls)), ("blocked", 0))


if __name__ == "__main__":
    unittest.main()
