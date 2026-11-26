"""Timeout-bounded command execution with captured, redacted logs. No shell is ever used."""
import os
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

from .util import redact

MAX_TEXT = 2_000_000
if os.name == "nt":
    # Unattended runs: a missing DLL or a crash must become an exit code, not a dialog on the user's screen.
    # Child processes inherit this error mode.
    import ctypes
    ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)


def _kill_own_tree(proc):
    """Stop only the process this module started, and its children."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True, timeout=30)
    else:
        import signal
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
    try:
        proc.kill()
    except OSError:
        pass


def resolve_program(program, env):
    if os.path.dirname(program):
        return program if os.path.isfile(program) else None
    path = next((v for k, v in env.items() if k.upper() == "PATH"), None)
    return shutil.which(program, path=path)


def run_process(argv, cwd, timeout, env, log_path=None, stdin_text=None):
    """Returns {exit_code, timed_out, error, output, duration_s, log}. exit_code is None if it never finished."""
    started = time.time()
    res = {"exit_code": None, "timed_out": False, "error": None, "output": "", "duration_s": 0.0, "log": None}
    program = resolve_program(argv[0], env)
    if program is None:
        res["error"] = f"program not found: {argv[0]}"
    else:
        # Output goes to a file, not a pipe: a surviving grandchild cannot block the timeout.
        with tempfile.TemporaryFile() as out:
            try:
                proc = subprocess.Popen(
                    [program] + list(argv[1:]), cwd=str(cwd), env=env, stdout=out, stderr=subprocess.STDOUT,
                    stdin=subprocess.PIPE if stdin_text is not None else subprocess.DEVNULL,
                    start_new_session=os.name != "nt")
                try:
                    proc.communicate(stdin_text.encode() if stdin_text is not None else None, timeout=timeout)
                    res["exit_code"] = proc.returncode
                except subprocess.TimeoutExpired:
                    res["timed_out"] = True
                    res["error"] = f"timed out after {timeout}s"
                    _kill_own_tree(proc)
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        res["error"] += " (process did not exit after kill)"
                except BaseException:
                    _kill_own_tree(proc)
                    raise
            except OSError as e:
                res["error"] = f"could not start: {e}"
            out.seek(0, os.SEEK_END)
            size = out.tell()
            out.seek(max(0, size - MAX_TEXT))
            res["output"] = redact(out.read().decode("utf-8", errors="replace").replace("\r\n", "\n"))
    res["duration_s"] = round(time.time() - started, 2)
    if log_path:
        log_path = Path(log_path)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        header = (f"$ {redact(subprocess.list2cmdline([str(a) for a in argv]))}\ncwd: {cwd}\n"
                  f"exit: {res['exit_code']}  timed_out: {res['timed_out']}  duration: {res['duration_s']}s\n"
                  f"error: {res['error']}\n\n")
        log_path.write_text(header + res["output"], encoding="utf-8")
        res["log"] = str(log_path)
    return res
