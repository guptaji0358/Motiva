"""Execution engine: dependency scheduling, permission gates, checkpoints, resume and evidence reuse."""
import os
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait

from . import planner, policy
from .reasoner import EscalateToOwner
from .report import write_report
from .step_runner import evidence_key, run_step
from .store import (BLOCKED, CANCELLED, COMPLETED, FAILED, RUNNING, Store, new_step_record, source_revision)
from .util import now

DONE = {"verified", "reused", "not_applicable"}
TERMINAL = DONE | {"failed", "unverified", "blocked", "skipped_dependency"}
BASELINES = {"git_snapshot", "registry_snapshot"}


class Engine:
    def __init__(self, paths, reasoner=None, retry_delay=1.0):
        self.paths = paths
        self.store = Store(paths)
        self.reasoner = reasoner or EscalateToOwner()
        self.retry_delay = retry_delay

    # ---- owner operations
    def submit(self, raw, scratch=None, grants=(), rerun=False):
        spec = planner.normalise(raw, self.paths, scratch, grants)
        planner.build_plan(spec, self.paths)  # reject impossible plans before storing them
        task, duplicate = self.store.submit(spec, self.paths.root, rerun)
        if duplicate and set(spec["grants"]) - set(task["spec"]["grants"]):
            task["duplicate_note"] = "grants of this resubmission were NOT applied; use the grant command"
        return task, duplicate

    def plan(self, tid):
        task = self.store.load(tid)
        return task, planner.build_plan(task["spec"], self.paths)

    def approve(self, tid, step_id, digest_, note=""):
        """Single-use approval of one step exactly as planned, for this task only."""
        with self.store.lock("run"):
            task, plan = self.plan(tid)
            entry = next((p for p in plan if p["id"] == step_id), None)
            if entry is None:
                raise ValueError(f"no such step: {step_id}")
            if entry["permission"] != policy.APPROVAL:
                raise ValueError(f"step {step_id} does not take an approval (it is {entry['permission']})")
            if digest_ != entry["digest"]:
                raise ValueError("digest does not match the planned step; run 'plan' and review it again")
            task["approvals"][step_id] = {"digest": digest_, "at": now(), "note": note, "used": False}
            self.store.event(task, f"approval recorded for step {step_id} ({digest_}) {note}".strip())
            self.store.save(task)
            return entry

    def grant(self, tid, grant):
        if grant not in policy.FIXED_GRANTS and not grant.startswith(("automation:", "script:")):
            raise ValueError(f"not a grantable permission: {grant}")
        with self.store.lock("run"):
            task = self.store.load(tid)
            if grant not in task["spec"]["grants"]:
                task["spec"]["grants"].append(grant)
                self.store.event(task, f"grant added: {grant}")
                self.store.save(task)
            return task

    def cancel(self, tid):
        with self.store.lock("run"):
            task = self.store.load(tid)
            if task["state"] == COMPLETED:
                raise ValueError("a completed task cannot be cancelled")
            task["state"] = CANCELLED
            self.store.event(task, "cancelled")
            self.store.save(task)
            return task

    # ---- execution
    def run(self, tid, fresh=False):
        """Run or resume a task. One task runs per project at a time (they share build directories)."""
        with self.store.lock("run"):
            task = self.store.load(tid)
            if task["state"] in (COMPLETED, CANCELLED):
                return task
            plan = {p["id"]: p for p in planner.build_plan(task["spec"], self.paths)}
            steps = {s["id"]: s for s in task["spec"]["steps"]}
            self._recover(task, plan, steps)
            task["runs"] += 1
            task["state"] = RUNNING
            task["revision"] = source_revision(self.paths.root)  # checkpoint for an interrupted run
            self.store.event(task, f"run {task['runs']} started" + (" (fresh: no evidence reuse)" if fresh else ""))
            self.store.save(task)
            try:
                self._schedule(task, plan, steps, fresh)
            except BaseException as e:
                self.store.event(task, f"interrupted: {type(e).__name__}")
                self.store.save(task)  # running steps stay 'running' and are judged on resume
                raise
            task["revision"] = source_revision(self.paths.root)
            task["state"] = self._final_state(task, steps)
            self.store.event(task, f"run {task['runs']} ended: {task['state']}")
            self.store.save(task)
            write_report(self.store, task, plan)
            return task

    def _recover(self, task, plan, steps):
        changed = task["revision"] is not None and task["revision"] != source_revision(self.paths.root)
        if changed:
            self.store.event(task, "sources changed since the last run: earlier results are re-verified")
        for sid, rec in task["steps"].items():
            step, state = steps[sid], rec["state"]
            if state == "running":
                if plan[sid]["permission"] == policy.APPROVAL:
                    self.store.event(task, f"{sid} was interrupted; outcome unknown, new approval required")
                    task["approvals"].pop(sid, None)
                else:
                    self.store.event(task, f"{sid} was interrupted; it is safe to run again")
                task["steps"][sid] = new_step_record()
            elif state in TERMINAL - DONE:
                rec["state"], rec["note"] = "pending", None
            elif state in DONE:
                baseline = step["action"] in BASELINES and not step["params"].get("baseline")
                final_check = bool(step["params"].get("baseline"))
                side_effect = plan[sid]["permission"] == policy.APPROVAL  # never repeated without approval
                if final_check or (changed and not baseline and not side_effect):
                    rec["state"], rec["reused_from"] = "pending", None

    def _gate(self, task, entry):
        """None if the step may run now, else the reason it is blocked."""
        if entry["permission"] == policy.APPROVAL:
            a = task["approvals"].get(entry["id"])
            if not a or a["used"] or a["digest"] != entry["digest"]:
                return f"approval required: {entry['reason']} (digest {entry['digest']})"
        elif entry["blocked_on"]:
            return f"permission '{entry['grant']}' not granted: {entry['reason']}"
        return None

    def _schedule(self, task, plan, steps, fresh):
        records = task["steps"]
        log_dir = str(self.store.task_dir(task["id"]) / "logs")
        os.makedirs(log_dir, exist_ok=True)
        may_recover = "recovery" in task["spec"]["grants"]
        running, held, keys = {}, set(), {}

        def resources(s):
            r = set(s["resources"])
            if s["params"].get("build_dir"):
                r.add("build:" + os.path.normcase(s["params"]["build_dir"]))
            if s["action"] == "edit_file":
                r.add("*")
            return r

        with ThreadPoolExecutor(max_workers=task["spec"]["max_parallel"]) as pool:
            while True:
                progressed = False
                for sid, step in steps.items():
                    rec = records[sid]
                    if rec["state"] != "pending":
                        continue
                    if any(records[d]["state"] not in TERMINAL for d in step["depends_on"] + step["run_after"]):
                        continue
                    bad = [d for d in step["depends_on"] if records[d]["state"] not in DONE]
                    if bad:
                        self._settle(task, sid, "skipped_dependency", f"not run: {', '.join(bad)} did not succeed")
                        progressed = True
                        continue
                    missing = [p for p in step["when_exists"]
                               if not os.path.exists(os.path.join(str(self.paths.root), p))]
                    if missing:
                        self._settle(task, sid, "not_applicable", f"not relevant: {', '.join(missing)} absent")
                        progressed = True
                        continue
                    reason = self._gate(task, plan[sid])
                    if reason:
                        self._settle(task, sid, "blocked", reason)
                        progressed = True
                        continue
                    need = resources(step)
                    if running and (need & held or "*" in need or "*" in held):
                        continue
                    if len(running) >= task["spec"]["max_parallel"]:
                        continue
                    keys[sid] = evidence_key(step, self.paths.root) if step["cacheable"] else None
                    ev = None if fresh or not keys[sid] else self.store.evidence_get(keys[sid])
                    if ev:
                        rec.update(state="reused", verification=ev["verification"], data=ev["data"],
                                   reused_from={"task": ev["task"], "when": ev["when"], "key": keys[sid]},
                                   note="identical definition and inputs already verified")
                        self.store.event(task, f"{sid}: reused evidence from {ev['task']}")
                        self.store.save(task)
                        progressed = True
                        continue
                    if sid in task["approvals"]:
                        task["approvals"][sid]["used"] = True
                    rec["state"] = "running"
                    self.store.event(task, f"{sid}: started")
                    self.store.save(task)  # checkpoint BEFORE the side effect
                    ctx = {"paths": self.paths, "root": str(self.paths.root), "log_dir": log_dir,
                           "run_no": task["runs"], "retry_delay": self.retry_delay,
                           "results": {k: {"data": v["data"]} for k, v in records.items() if v["state"] in DONE}}
                    future = pool.submit(run_step, step, ctx, plan[sid]["permission"], may_recover)
                    running[future] = (sid, need)
                    held |= need
                    progressed = True
                if progressed:
                    continue
                if not running:
                    break
                finished, _ = wait(running, return_when=FIRST_COMPLETED)
                for fut in finished:
                    sid, need = running.pop(fut)
                    held -= need
                    self._record(task, steps[sid], fut.result(), keys.get(sid))

    def _settle(self, task, sid, state, note):
        task["steps"][sid].update(state=state, note=note)
        self.store.event(task, f"{sid}: {state}: {note}")
        self.store.save(task)

    def _record(self, task, step, result, key):
        sid = step["id"]
        for d in result["diagnoses"]:
            fixed = d["fix"] and d["fix"]["result"] == "verified"
            d["seen_before"] = self.store.diagnosis_history(d, task["id"])
            if fixed:
                self.store.diagnosis_history(d, task["id"], fixed_by=d["fix"]["applied"]["type"])
        if result["state"] != "verified" and result["diagnoses"]:
            result["advice"] = self.reasoner.advise(step, result["diagnoses"][-1])
        else:
            result["advice"] = None
        task["steps"][sid].update(result, note=None, reused_from=None)
        self.store.event(task, f"{sid}: {result['state']} after {len(result['attempts'])} attempt(s)")
        # Store evidence only if the inputs did not change while the check ran.
        if key and result["state"] == "verified" and evidence_key(step, self.paths.root) == key:
            self.store.evidence_put(key, task["id"], task["steps"][sid])
        self.store.save(task)

    @staticmethod
    def _final_state(task, steps):
        required = [task["steps"][s]["state"] for s in steps if not steps[s]["optional"]]
        if "failed" in required:
            return FAILED
        if any(s not in DONE for s in required):
            return BLOCKED
        return COMPLETED
