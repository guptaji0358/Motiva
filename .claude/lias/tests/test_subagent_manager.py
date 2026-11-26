"""Tests for claude-subagent-manager. Run: python .claude/lias/tests/test_subagent_manager.py"""
import importlib.util
import json
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "automation" / "scripts" / "claude_subagent_manager.py"
spec_ = importlib.util.spec_from_file_location("csm", SCRIPT)
csm = importlib.util.module_from_spec(spec_)
spec_.loader.exec_module(csm)

REAL_TEAM = Path(__file__).resolve().parents[1] / "agent-teams" / "motiva-team.json"


def good(**kw):
    s = {"name": "demo-reader", "description": "Reads things.", "role": "read-only", "objective": "Read.",
         "instructions": ["Read a file."]}
    s.update(kw)
    return s


class T(unittest.TestCase):
    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="csm-"))

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def put(self, rel, obj):
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(json.dumps(obj), encoding="utf-8")

    def run_(self, **args):
        try:
            return csm.execute(str(self.root), args)
        except csm.SpecError as e:
            return {"ok": False, "error": "INVALID", "message": str(e)}

    def snapshot(self):
        return sorted(p.relative_to(self.root).as_posix() for p in self.root.rglob("*"))

    def test_create_and_validate(self):
        self.put("spec.json", good())
        r = self.run_(action="create", spec_file="spec.json")
        self.assertTrue(r["ok"] and r["status"] == "written")
        f = self.root / ".claude/agents/demo-reader.md"
        text = f.read_text(encoding="utf-8")
        fm = csm.parse_frontmatter(text)
        self.assertEqual(fm["name"], "demo-reader")
        self.assertEqual(fm["tools"], "Glob, Grep, Read")
        self.assertIn("Do not spawn other agents", text)
        self.assertTrue(self.run_(action="validate", spec_file="spec.json")["ok"])

    def test_dry_run_writes_nothing(self):
        self.put("spec.json", good())
        before = self.snapshot()
        r = self.run_(action="create", spec_file="spec.json", dry_run=True)
        self.assertEqual(r["status"], "dry-run")
        self.assertEqual(before, self.snapshot())
        self.assertFalse((self.root / ".claude/agents").exists())

    def test_overwrite_protection_and_content_untouched(self):
        self.put("spec.json", good())
        self.run_(action="create", spec_file="spec.json")
        f = self.root / ".claude/agents/demo-reader.md"
        f.write_text("HAND EDITED", encoding="utf-8")
        r = self.run_(action="create", spec_file="spec.json")
        self.assertFalse(r["ok"])
        self.assertEqual(r["error"], "EXISTS")
        self.assertEqual(f.read_text(encoding="utf-8"), "HAND EDITED")

    def test_team_all_or_nothing_on_conflict(self):
        self.put("t.json", {"team": "x", "agents": [good(name="aaa-one"), good(name="bbb-two")]})
        (self.root / ".claude/agents").mkdir(parents=True)
        (self.root / ".claude/agents/bbb-two.md").write_text("keep", encoding="utf-8")
        r = self.run_(action="team", team_file="t.json")
        self.assertEqual(r["error"], "EXISTS")
        self.assertFalse((self.root / ".claude/agents/aaa-one.md").exists())
        self.assertEqual((self.root / ".claude/agents/bbb-two.md").read_text(encoding="utf-8"), "keep")

    def test_duplicate_names_in_team(self):
        self.put("t.json", {"team": "x", "agents": [good(), good()]})
        r = self.run_(action="team", team_file="t.json")
        self.assertIn("duplicate", r["message"])

    def test_malformed_input(self):
        (self.root / "bad.json").write_text("{not json", encoding="utf-8")
        self.assertIn("not valid JSON", self.run_(action="validate", spec_file="bad.json")["message"])
        self.assertIn("not found", self.run_(action="validate", spec_file="missing.json")["message"])
        self.assertFalse(self.run_(action="validate")["ok"])
        self.assertFalse(self.run_(action="bogus")["ok"])
        for bad in (good(name="Bad Name"), good(name="ab"), good(role="root"), good(description=""),
                    good(instructions=[]), good(model="gpt"), good(extra=1), good(description="a\nb"),
                    good(objective=5), good(tools="Read")):
            self.put("s.json", bad)
            self.assertFalse(self.run_(action="validate", spec_file="s.json")["ok"], bad)
        self.put("t.json", {"team": "x", "agents": []})
        self.assertFalse(self.run_(action="team", team_file="t.json")["ok"])

    def test_invalid_and_forbidden_tools(self):
        for tools, frag in ((["Read", "Nope"], "invalid tool"), (["Agent"], "forbidden"), (["Task"], "forbidden"),
                            (["Edit"], "not allowed for role"), (["Bash"], "not allowed for role"),
                            (["Read", "Read"], "duplicates")):
            self.put("s.json", good(tools=tools))
            r = self.run_(action="validate", spec_file="s.json")
            self.assertFalse(r["ok"])
            self.assertIn(frag, r["message"])

    def test_roles(self):
        self.put("s.json", good(name="impl-agent", role="implementation"))
        self.assertIn("owned_files", self.run_(action="validate", spec_file="s.json")["message"])
        self.put("s.json", good(name="impl-agent", role="implementation", owned_files=["scripts/src/a.cpp"]))
        r = self.run_(action="create", spec_file="s.json")
        self.assertTrue(r["ok"])
        text = (self.root / ".claude/agents/impl-agent.md").read_text(encoding="utf-8")
        self.assertIn("Edit", text.split("---")[1])
        self.assertIn("`scripts/src/a.cpp`", text)
        self.put("s.json", good(role="verifier", owned_files=["a.cpp"]))
        self.assertFalse(self.run_(action="validate", spec_file="s.json")["ok"])
        self.put("s.json", good(name="verif-agent", role="verifier"))
        self.run_(action="create", spec_file="s.json")
        t = (self.root / ".claude/agents/verif-agent.md").read_text(encoding="utf-8")
        self.assertNotIn("Edit", t.split("---")[1])

    def test_unsafe_paths(self):
        for p in ("../outside.json", "/abs/x.json", "C:\\x.json", "a/../../x.json", "\\x.json"):
            r = self.run_(action="validate", spec_file=p)
            self.assertFalse(r["ok"], p)
        for p in ("../x.cpp", "/etc/x", "keys/dev_private_key.pem"):
            self.put("s.json", good(name="impl-agent", role="implementation", owned_files=[p]))
            self.assertFalse(self.run_(action="validate", spec_file="s.json")["ok"], p)
        self.assertEqual(self.snapshot(), ["s.json"])

    def test_list(self):
        self.put("spec.json", good())
        self.run_(action="create", spec_file="spec.json")
        r = self.run_(action="list")
        self.assertEqual([a["name"] for a in r["agents"]], ["demo-reader"])

    def test_real_motiva_team(self):
        team = json.loads(REAL_TEAM.read_text(encoding="utf-8"))
        self.put("team.json", team)
        r = self.run_(action="team", team_file="team.json")
        self.assertTrue(r["ok"], r)
        names = sorted(a["name"] for a in r["agents"])
        self.assertEqual(names, ["motiva-architecture-safety", "motiva-browser-integration",
                                 "motiva-desktop-integration", "motiva-regression-testing",
                                 "motiva-release-readiness"])
        arch = (self.root / ".claude/agents/motiva-architecture-safety.md").read_text(encoding="utf-8")
        self.assertEqual(csm.parse_frontmatter(arch)["tools"], "Glob, Grep, Read")


if __name__ == "__main__":
    unittest.main()
