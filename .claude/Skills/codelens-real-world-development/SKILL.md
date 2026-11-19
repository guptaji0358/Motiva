---
name: codelens-real-world-development
description: Use the user's Code-Lens MCP as a practical code-intelligence tool during real software-development work across projects. Discover bugs, limitations, compatibility issues, and improvement opportunities through actual use; discuss meaningful Code-Lens improvements before implementing them.
---

# Code-Lens: Real-World Development Workflow

## Purpose

Code-Lens is the user's own MCP server for AI coding agents. It is a tool to use during real software-development work, not a project to recreate or modify whenever the user asks for help with another repository.

Apply this workflow across **all projects and repositories**, not just one named project.

## Core rule

**Do real project work first. Use Code-Lens where it helps. Let real problems reveal what Code-Lens should become.**

Do not invent a feature roadmap or implement Code-Lens improvements merely because an idea sounds useful. The user and agent improve Code-Lens together based on evidence from actual development tasks.

## 1. Discover and use Code-Lens

At the start of a substantive coding task:

1. Check whether the Code-Lens MCP server and its tools are available in the current agent environment.
2. If available, use the relevant Code-Lens tools to inspect the target repository: search code, read relevant ranges, inspect file outlines and symbols, find references, and gather repository context as appropriate.
3. Use Code-Lens as an aid, not as the sole source of truth. Verify important conclusions against source files, compiler output, tests, runtime behavior, and authoritative project documentation.
4. Choose tools based on the task. Do not call every tool mechanically or generate unnecessary output.
5. If Code-Lens is unavailable or disconnects, continue the user's task with other reliable methods when possible. Clearly report that Code-Lens could not be used; do not pretend it was used, and do not derail the task into rebuilding it.

Never claim access to Code-Lens unless its MCP tools are actually present and callable in this session.

## 2. Keep the user's actual task primary

- Work on the project the user asked about.
- Do not turn an ordinary bug fix, feature request, review, or investigation into a Code-Lens development task.
- Do not create another Code-Lens copy, move its installation, or change its configuration unless the user explicitly asks.
- Do not modify Code-Lens during unrelated project work without the user's agreement.
- Do not let exploratory Code-Lens evaluation block delivery of the requested project work.

## 3. Learn from real usage

While using Code-Lens, notice evidence-backed issues such as:

- Incorrect, incomplete, misleading, or stale results.
- Missing repository or language context.
- Bugs, crashes, timeouts, disconnections, or unreliable behavior.
- Excessive output, unnecessary token usage, or poor usability.
- Slow operations or inefficient workflows.
- Compatibility problems involving languages, frameworks, libraries, APIs, compilers, build systems, or versions.
- Repeated manual steps that a useful MCP capability could safely simplify.
- Existing features that work but could be more accurate, reliable, fast, or understandable.

Do not manufacture problems to justify new features. Distinguish confirmed defects from hypotheses and untested ideas.

## 4. Report useful findings to the user

When a meaningful issue or opportunity is discovered, explain it concisely:

1. **Observed situation:** What task was being performed?
2. **Evidence:** Which Code-Lens tool/result, file, error, or behavior revealed it?
3. **Impact:** How did it affect the real development task?
4. **Classification:** Bug, limitation, usability issue, performance issue, compatibility issue, or feature opportunity.
5. **Suggested improvement:** The smallest practical change that could address it.
6. **Confidence:** Confirmed, likely, or unverified.

Do not interrupt the user for every minor observation. Group related findings and raise important ones at a sensible point, especially when they affect correctness, reliability, security, or task completion.

## 5. Discuss before changing Code-Lens

- Do not implement meaningful Code-Lens improvements automatically.
- Present the real problem, evidence, proposed solution, trade-offs, and test approach.
- Ask the user whether they want the change before making major changes to Code-Lens.
- For a tiny, clearly authorized maintenance fix, follow the user's explicit instructions and normal project workflow.
- Avoid duplicating capabilities that already exist. Inspect the current architecture and implementation before proposing changes.
- After approval and implementation, test the change and report what was actually verified.

## 6. Private evaluation notes

If the user wants evaluation notes or ratings retained:

- Store them in a local private directory such as `.private/evaluations/` within the relevant working repository, unless the user specifies another location.
- Ensure the private directory is excluded by `.gitignore`.
- Check `git status` and the ignore rule before committing or publishing.
- Never commit, push, publish, or expose private ratings, evaluation notes, credentials, or other private data.
- Do not silently change `.gitignore` or commit changes unless authorized by the user.
- Ratings are the agent's own evidence-based judgments, not Code-Lens-generated scores. Label them accordingly.
- Record what was tested, what worked, what failed, limitations, evidence, proposed improvements, and whether an approved change was implemented and verified.
- Do not claim notes were saved unless the file was actually written and verified.

## 7. Evaluation discipline

Evaluate only capabilities actually exercised during real work. For each capability, record:

- Task and context.
- Tool and operation used.
- Expected behavior.
- Actual behavior.
- Result and evidence.
- Rating, if useful, with a short rationale.
- Unresolved questions.

Do not rate unused tools. Mark them as **not evaluated** and try them on a suitable future task.

## 8. What good behavior looks like

- User asks to fix a bug in Project A → use Code-Lens to investigate Project A when available; fix and verify Project A's bug.
- During that work, Code-Lens returns an incorrect reference → document the evidence and discuss a possible Code-Lens fix with the user.
- User asks for compatibility analysis in Project B → use Code-Lens to inspect relevant code and dependencies; verify version claims against package metadata and official sources as appropriate.
- Code-Lens is unavailable → report that honestly and continue with other tools if possible.
- User asks to improve Code-Lens itself → then treat Code-Lens as the target project and follow the user's requested scope.

## Guiding principle

**Use Code-Lens across real projects. Discover real problems through experience. Discuss improvements with the user. Build only what real evidence and the user's decision justify.**
