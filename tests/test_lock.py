"""Test 39: one run at a time in one results.hashdiff (section 3.6).

A helper process holds an advisory write lock on results.hashdiff/lock, which is the same kind
of lock hashdiff takes, so the two really do exclude each other.
"""
import fcntl
import os
import signal
import subprocess
import sys
import time

import pytest

from conftest import run_on_pty

SPEC = {"a": b"aa", "b": b"bb"}

HOLDER = """
import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o666)
try:
    fcntl.lockf(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
except OSError:
    sys.stdout.write("no\\n")
    sys.stdout.flush()
    sys.exit(1)
sys.stdout.write("%d\\n" % os.getpid())
sys.stdout.flush()
time.sleep(300)
"""


class Holder:
    """A separate process holding the lock, so hashdiff sees a real pid."""

    def __init__(self, path):
        self.proc = subprocess.Popen([sys.executable, "-c", HOLDER, os.fsdecode(path)],
                                     stdout=subprocess.PIPE)
        first = self.proc.stdout.readline().strip()
        if first == b"no":
            pytest.skip("this filesystem does not support fcntl locks")
        self.pid = int(first)

    def stop(self, sig=signal.SIGTERM):
        if self.proc.poll() is None:
            self.proc.send_signal(sig)
            self.proc.wait()


@pytest.fixture
def setup(run_hashdiff, make_tree, tmp_path):
    """A finished run, its results directory, and the arguments to run it again."""
    origin = make_tree("origin", SPEC)
    destination = make_tree("destination", SPEC)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    results = os.path.join(out, b"results.hashdiff")
    assert run_hashdiff(origin, destination, "-o", out)[0] == 0
    return [origin, destination, "-o", out, "--force"], os.path.join(results, b"lock")


def test_a_held_lock_stops_the_run(run_hashdiff, setup):
    args, lock = setup
    holder = Holder(lock)
    try:
        code, out, err = run_hashdiff(*args)
        assert code == 2
        assert b"is in use by process %d" % holder.pid in err
        assert b"--ignore-lock" in err
    finally:
        holder.stop()


def test_ignore_lock_runs_anyway(run_hashdiff, setup):
    args, lock = setup
    holder = Holder(lock)
    try:
        code, out, err = run_hashdiff(*args, "--ignore-lock")
        assert code == 0, err
        assert b"is in use" not in err
    finally:
        holder.stop()


def test_the_prompt_asks_before_continuing(hashdiff_bin, setup):
    args, lock = setup
    holder = Holder(lock)
    try:
        code, out, term = run_on_pty(hashdiff_bin, args, b"a\n")
        assert code == 2, term
        assert b"continue anyway? [y]es or [a]bort?" in term

        code, out, term = run_on_pty(hashdiff_bin, args, b"maybe\ny\n")
        assert code == 0, term
        assert term.count(b"continue anyway?") == 2
    finally:
        holder.stop()


def test_a_killed_holder_leaves_no_lock(run_hashdiff, setup):
    """The kernel releases it on every exit path, so there is no stale lock to reason about."""
    args, lock = setup
    holder = Holder(lock)
    holder.stop(signal.SIGKILL)

    code, out, err = run_hashdiff(*args)
    assert code == 0, err
    assert b"is in use" not in err
    assert os.path.exists(lock)                 # the file stays; the lock does not


def test_lock_survives_force_and_is_never_a_result_file(run_hashdiff, setup):
    args, lock = setup
    assert os.path.exists(lock)
    code, out, err = run_hashdiff(*args)
    assert code == 0, err
    assert os.path.exists(lock)


def test_two_runs_at_once_and_one_of_them_stops(run_hashdiff, make_tree, tmp_path,
                                                hashdiff_bin):
    """A fresh directory is as much at risk as one being overwritten (section 3.6)."""
    spec = {"f%03d" % i: os.urandom(20000) for i in range(120)}
    origin = make_tree("origin", spec)
    destination = make_tree("destination", spec)
    out = os.path.join(os.fsencode(tmp_path), b"out")
    os.makedirs(out, exist_ok=True)
    args = [hashdiff_bin, origin, destination, b"-o", out]

    procs = [subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE) for _ in range(2)]
    results = [p.communicate() for p in procs]
    codes = sorted(p.returncode for p in procs)
    if codes == [0, 0]:
        pytest.skip("the two runs did not overlap")
    assert codes == [0, 2], results
    stderr = b"".join(r[1] for r in results)
    assert b"is in use" in stderr or b"already exists" in stderr
