"""LIAS Autonomous Engineering Agent: a deterministic task worker (no LLM inside).

Claude (or a person) is the task owner: it submits a JSON task, the worker plans, executes, verifies,
recovers from recognised failures and writes a report. See ../docs/agent.md.
"""
AGENT_VERSION = "1.0.0"
