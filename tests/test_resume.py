"""Tests 12 and 17-22: resuming an interrupted run."""
import os
import signal
import subprocess
import time

import pytest

from conftest import RESULT_FILES, Symlink, read_bytes, run_on_pty, strip_time

DIFF_OUTPUTS = [b"diff-files.txt", b"rsync-files.lst", b"rsync-command.txt"]
# "# hashdiff-format:", "# root:", "# mode:" and "# started:" (section 8).
HEADER_LINES = 4


def spec(n=40):
    files = {"dir%d/file%03d" % (i % 3, i): os.urandom(100 + 37 * i) for i in range(n)}
    files["link"] = Symlink("dir0/file000")
    return files


def make_pair(make_tree, n=40, differ=True):
    files = spec(n)
    origin = make_tree("origin", files)
    if differ:
        files = dict(files)
        files["dir1/file001"] = b"X" * len(files["dir1/file001"])
    destination = make_tree("destination", files)
    return origin, destination


class Runs:
    """Runs hashdiff into one output directory and a reference one."""

    def __init__(self, run_hashdiff, tmp_path, origin, destination, args=()):
        self.run_hashdiff = run_hashdiff
        self.origin, self.destination, self.args = origin, destination, list(args)
        self.out = os.path.join(os.fsencode(tmp_path), b"out")
        self.ref = os.path.join(os.fsencode(tmp_path), b"ref")
        os.makedirs(self.out, exist_ok=True)
        os.makedirs(self.ref, exist_ok=True)
        self.results = os.path.join(self.out, b"results.hashdiff")

    def run(self, *extra, **kwargs):
        return self.run_hashdiff(self.origin, self.destination, "-o", self.out, *self.args,
                                 *extra, **kwargs)

    def files(self, out):
        results = os.path.join(out, b"results.hashdiff")
        files = {n: strip_time(read_bytes(os.path.join(results, n)))
                 for n in RESULT_FILES if n != b"history.txt"}
        files[b"rsync-command.txt"] = files[b"rsync-command.txt"].replace(out, b"OUT")
        return files

    def assert_identical_to_uninterrupted(self):
        code, out, err = self.run_hashdiff(self.origin, self.destination, "-o", self.ref,
                                           "--force", *self.args)
        assert code in (0, 1), err
        assert self.files(self.out) == self.files(self.ref)
        assert sorted(os.listdir(self.results)) == sorted(RESULT_FILES)

    def path(self, name):
        return os.path.join(self.results, name)

    def simulate_interruption(self, cuts):
        """Turns the hashes files into journals cut after a number of bytes per side."""
        for side, cut in cuts.items():
            final = self.path(b"hashes-%s.txt" % side)
            data = read_bytes(final)
            if cut is None:
                continue        # this side had finished
            os.remove(final)
            with open(final + b".tmp", "wb") as f:
                f.write(data[:cut])
        for name in DIFF_OUTPUTS:
            os.remove(self.path(name))


def line_offset(data, line_no, extra=0):
    """Byte offset of the start of a line (0-based), plus extra bytes."""
    pos = 0
    for _ in range(line_no):
        pos = data.index(b"\n", pos) + 1
    return pos + extra


def reused(stdout, side):
    for line in stdout.split(b"\n"):
        if line.startswith(b"resume " + side + b": "):
            return int(line.split()[2])
    raise AssertionError("no resume line for %r in %r" % (side, stdout))


def cut_both(runs, at=HEADER_LINES + 10):
    """Turns a finished run into an interrupted one, so there is something to resume."""
    data = {side: read_bytes(runs.path(b"hashes-%s.txt" % side))
            for side in (b"origin", b"destination")}
    runs.simulate_interruption({side: line_offset(data[side], at) for side in data})


def test_resume_refused_with_other_parameters(make_tree, run_hashdiff, tmp_path):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    # A finished run has nothing to resume, whatever the parameters are (section 3.3).
    code, out, err = runs.run("--resume")
    assert code == 2 and b"nothing to resume" in err and b"--force" in err
    cut_both(runs)
    code, out, err = runs.run("--resume", "--fast")
    assert code == 2 and b"--force" in err and b"mode" in err
    # paths.txt catches another root before the journals are even opened (section 3.3).
    other = make_tree("other", spec())
    code, out, err = run_hashdiff(other, destination, "-o", runs.out, "--resume")
    assert code == 2 and b"a different ORIGIN" in err and b"--force" in err
    code, out, err = runs.run("--resume", "--force")
    assert code == 2


@pytest.mark.parametrize("published", [None, b"origin"])
def test_simulated_interruption(make_tree, run_hashdiff, tmp_path, published):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    data = {side: read_bytes(runs.path(b"hashes-%s.txt" % side))
            for side in (b"origin", b"destination")}
    cuts = {b"origin": line_offset(data[b"origin"], HEADER_LINES + 30, 10),     # middle of a line
            b"destination": line_offset(data[b"destination"], HEADER_LINES + 12)}
    if published:
        cuts[published] = None
    runs.simulate_interruption(cuts)
    code, out, err = runs.run("--resume")
    assert code == 1, err
    # Each side keeps its own valid lines; the last kept one is hashed again.
    assert reused(out, b"origin") == (40 if published else 29)
    assert reused(out, b"destination") == 11
    runs.assert_identical_to_uninterrupted()


def test_wrong_hash_in_last_kept_line(make_tree, run_hashdiff, tmp_path):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    data = read_bytes(runs.path(b"hashes-origin.txt"))
    end = line_offset(data, HEADER_LINES + 20)
    last = line_offset(data, HEADER_LINES + 19)
    damaged = data[:last + 2] + b"0" * 32 + data[last + 34:end]
    runs.simulate_interruption({b"origin": None, b"destination": None})
    os.remove(runs.path(b"hashes-origin.txt"))
    with open(runs.path(b"hashes-origin.txt.tmp"), "wb") as f:
        f.write(damaged)
    code, out, err = runs.run("--resume")
    assert code == 1, err
    assert b"[origin] resume: last entry changed, re-hashed: " in err
    assert b"last kept entry re-hashed" in out
    runs.assert_identical_to_uninterrupted()


@pytest.mark.parametrize("corrupt", ["bad-hex", "unsorted", "bad-escape", "fields"])
def test_invalid_line_in_the_middle(make_tree, run_hashdiff, tmp_path, corrupt):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    data = read_bytes(runs.path(b"hashes-origin.txt"))
    lines = data.split(b"\n")
    bad = HEADER_LINES + 15
    if corrupt == "bad-hex":
        lines[bad] = lines[bad][:2] + b"Z" + lines[bad][3:]
    elif corrupt == "unsorted":
        lines[bad] = lines[bad - 5]
    elif corrupt == "bad-escape":
        lines[bad] = lines[bad] + b"\\q"
    else:
        lines[bad] = lines[bad].replace(b" ", b"", 1)
    runs.simulate_interruption({b"origin": None, b"destination": None})
    os.remove(runs.path(b"hashes-origin.txt"))
    with open(runs.path(b"hashes-origin.txt.tmp"), "wb") as f:
        f.write(b"\n".join(lines))
    code, out, err = runs.run("--resume")
    assert code == 1, err
    assert reused(out, b"origin") == 14
    runs.assert_identical_to_uninterrupted()


def test_trees_changed_after_interruption(make_tree, run_hashdiff, tmp_path, parsers):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    runs.simulate_interruption({
        b"origin": line_offset(read_bytes(runs.path(b"hashes-origin.txt")), HEADER_LINES + 20),
        b"destination": line_offset(read_bytes(runs.path(b"hashes-destination.txt")), HEADER_LINES + 10)})
    saved = {n: read_bytes(runs.path(n)) for n in os.listdir(runs.results)}

    def path(root, rel):
        return os.path.join(root, rel)

    deleted, resized, touched = path(origin, b"dir0/file003"), path(origin, b"dir1/file004"), \
        path(destination, b"dir2/file005")
    created = path(destination, b"dir0/new-file")
    keep = {p: (read_bytes(p), os.stat(p)) for p in (deleted, resized, touched)}
    os.remove(deleted)
    with open(resized, "ab") as f:
        f.write(b"more")
    os.utime(touched, (keep[touched][1].st_atime, keep[touched][1].st_mtime + 100))
    with open(created, "wb") as f:
        f.write(b"new")

    code, out, err = runs.run("--resume")
    assert code == 2
    assert b"the trees changed since the interrupted run" in err
    _, sections = parsers.report(runs.path(b"tree-changes.txt"))
    assert sections[b"origin"] == [(b"DELETED", b"dir0/file003"), (b"CHANGED", b"dir1/file004")]
    assert sections[b"destination"] == [(b"ADDED", b"dir0/new-file"),
                                        (b"CHANGED", b"dir2/file005")]
    after = {n: read_bytes(runs.path(n)) for n in os.listdir(runs.results)}
    del after[b"tree-changes.txt"]
    assert after == saved

    os.remove(created)
    for p, (content, st) in keep.items():
        with open(p, "wb") as f:
            f.write(content)
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
    code, out, err = runs.run("--resume")
    assert code == 1, err
    runs.assert_identical_to_uninterrupted()


def test_prompt(make_tree, run_hashdiff, hashdiff_bin, tmp_path):
    origin, destination = make_pair(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination)
    assert runs.run()[0] == 1
    cut_both(runs)
    args = [origin, destination, "-o", runs.out]
    prompt = b"holds an interrupted run started on "
    before = {n: read_bytes(runs.path(n)) for n in os.listdir(runs.results)}

    code, out, term = run_on_pty(hashdiff_bin, args, b"a\n")
    assert code == 2 and prompt in term
    assert {n: read_bytes(runs.path(n)) for n in os.listdir(runs.results)} == before

    code, out, term = run_on_pty(hashdiff_bin, args, b"maybe\nr\n")
    assert code == 1, term
    assert term.count(prompt) == 2
    assert b"resume origin: " in out

    code, out, term = run_on_pty(hashdiff_bin, args, b"o\n")
    assert code == 1, term
    assert b"resume origin: " not in out
    runs.assert_identical_to_uninterrupted()


def slow_trees(make_tree):
    """Small files first, then a sparse file that takes seconds to read in full."""
    files = {"a/f%03d" % i: os.urandom(100) for i in range(50)}
    origin = make_tree("origin", files)
    destination = make_tree("destination", files)
    size = 2 * 1024 ** 3
    for root in (origin, destination):
        path = os.path.join(root, b"z-big")
        try:
            with open(path, "wb") as f:
                f.truncate(size)
        except OSError as e:
            pytest.skip("cannot create a sparse file: %s" % e)
        if os.stat(path).st_blocks * 512 >= size:
            pytest.skip("the filesystem does not support sparse files")
    return origin, destination


def wait_for_lines(path, count, proc, timeout=60):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            return False
        try:
            if read_bytes(path).count(b"\n") >= count:
                return True
        except OSError:
            pass
        time.sleep(0.01)
    return False


@pytest.mark.parametrize("jobs", ["1", "4"])
def test_real_interruption(make_tree, run_hashdiff, hashdiff_bin, tmp_path, parsers, jobs):
    origin, destination = slow_trees(make_tree)
    runs = Runs(run_hashdiff, tmp_path, origin, destination, ["-j", jobs])
    journal = runs.path(b"hashes-origin.txt.tmp")
    proc = subprocess.Popen([hashdiff_bin, origin, destination, "-o", runs.out, "-j", jobs],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    if not wait_for_lines(journal, HEADER_LINES + 50, proc):
        proc.kill()
        proc.communicate()
        pytest.skip("the run finished before it could be interrupted")
    proc.send_signal(signal.SIGINT)
    stdout, stderr = proc.communicate(timeout=120)
    assert proc.returncode == 130, stderr
    assert b"interrupted" in stderr
    names = set(os.listdir(runs.results))
    assert b"hashes-origin.txt.tmp" in names and b"hashes-origin.txt" not in names
    assert not any(n.startswith(b"diff-files") or n.startswith(b"rsync") for n in names)
    _, entries = parsers.hashes(journal)
    assert [e[3] for e in entries][:50] == [b"a/f%03d" % i for i in range(50)]

    code, out, err = runs.run("--resume")
    assert code == 0, err
    assert reused(out, b"origin") >= 49
    runs.assert_identical_to_uninterrupted()
