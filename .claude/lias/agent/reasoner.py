"""Replaceable reasoning interface.

The shipped reasoner does no reasoning: it escalates to the task owner. A future model integration would
subclass Reasoner and be approved explicitly. Whatever it returns is ADVICE shown in the report: the
engine never executes a reasoner's output, so it cannot widen permissions.
"""


class Reasoner:
    name = "base"

    def advise(self, step, diagnosis):
        """Return a short recommendation string for the task owner."""
        raise NotImplementedError


class EscalateToOwner(Reasoner):
    name = "escalate-to-owner"
    HINTS = {
        "test-failures": "Read the failing assertions, decide whether the code or the test is wrong, then resubmit.",
        "compile-or-link-error": "Fix the listed source errors, then resubmit; unchanged verified steps are reused.",
        "cmake-error": "Inspect the configure log and correct the CMake or toolchain settings.",
        "python-module-missing": "Decide whether to install the module, then resume the task.",
        "cmake-cache-mismatch": "Resubmit with a new --scratch directory.",
        "unrecognised": "Unknown failure: inspect the step log and decide how to proceed.",
    }

    def advise(self, step, diagnosis):
        return self.HINTS.get(diagnosis["pattern"],
                              f"Step '{step['id']}' needs your judgment: {diagnosis['summary']}")
